/* git-revroot.c --- a git commit mapped as a revision root
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <apr_general.h>
#include <apr_pools.h>

#include "svn_fs.h"
#include "svn_hash.h"
#include "svn_dirent_uri.h"
#include "svn_version.h"
#include "svn_pools.h"

#include "svn_private_config.h"

#include "private/svn_fs_util.h"

#include "../libsvn_fs/fs-loader.h"
#include "fs_git.h"

typedef struct svn_fs_git_root_t
{
  git_commit *commit;
  const char *rev_path;
  svn_boolean_t exact;
  apr_hash_t *branch_map;
} svn_fs_git_root_t;

typedef struct svn_fs_git_fs_id_t
{
  git_oid commit;
  const char *path;
  const char *branch;
  svn_fs_root_t *root;
} svn_fs_git_fs_id_t;

/* We don't have real ids (yet) */
static svn_string_t *fs_git_id_unparse(const svn_fs_id_t *id,
                                       apr_pool_t *pool)
{
  return svn_string_create("", pool);
}

/* Fake an id via the node relation check to make the repos layer
   happy */
static svn_fs_node_relation_t fs_git_id_compare(const svn_fs_id_t *a,
                                                const svn_fs_id_t *b)
{
  const svn_fs_git_fs_id_t *id_a, *id_b;
  if (a->vtable != b->vtable)
    return svn_fs_node_unrelated;

  id_a = a->fsap_data;
  id_b = b->fsap_data;

  if (id_a->root && id_b->root
      && id_a->root->fs == id_b->root->fs)
    {
      svn_fs_node_relation_t rel;
      svn_error_t *err;

      err = id_a->root->vtable->node_relation(&rel,
                                              id_a->root, id_a->path,
                                              id_b->root, id_b->path,
                                              id_a->root->pool);

      if (!err)
        return rel;

      svn_error_clear(err);
    }

  return svn_fs_node_unrelated;
}

static id_vtable_t id_vtable =
{
  fs_git_id_unparse,
  fs_git_id_compare
};

static svn_fs_id_t *
make_id(svn_fs_root_t *root,
        const char *path,
        apr_pool_t *result_pool)
{
  svn_fs_git_fs_id_t *id = apr_pcalloc(result_pool, sizeof(*id));
  svn_fs_id_t *fsid = apr_pcalloc(result_pool, sizeof(*fsid));

  fsid->fsap_data = id;
  fsid->vtable = &id_vtable;

  id->path = apr_pstrdup(result_pool, path);
  id->root = root;
  memset(&id->commit, 0, sizeof(id->commit));

  return fsid;
}

static const char *
make_fspath(const char *relpath, apr_pool_t *result_pool)
{
  if (*relpath == '/')
    relpath++;

  return apr_pstrcat(result_pool, "/", relpath, SVN_VA_NULL);
}

/* svn_relpath_split, but then for the first component instead of the last */
static svn_error_t *
relpath_reverse_split(const char **root, const char **remaining,
                      const char *relpath,
                      apr_pool_t *result_pool)
{
  const char *ch = strchr(relpath, '/');
  SVN_ERR_ASSERT(svn_relpath_is_canonical(relpath));

  if (!ch)
    {
      if (root)
        *root = apr_pstrdup(result_pool, relpath);
      if (remaining)
        *remaining = "";
    }
  else
    {
      if (root)
        *root = apr_pstrmemdup(result_pool, relpath, ch - relpath);
      if (remaining)
        *remaining = apr_pstrdup(result_pool, ch + 1);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
find_commit(const git_commit **commit, svn_fs_root_t *root,
            const git_oid *oid, apr_pool_t *result_pool)
{
  svn_fs_git_fs_t *fgf = root->fs->fsap_data;

  return svn_error_trace(
    svn_git__commit_lookup(commit, fgf->repos, oid, result_pool));
}

static svn_error_t *
find_tree_entry(const git_tree_entry **entry, git_tree *tree,
                const char *relpath,
                apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  const char *basename, *tail;
  const git_tree_entry *e;

  SVN_ERR(relpath_reverse_split(&basename, &tail, relpath, scratch_pool));

  e = git_tree_entry_byname(tree, basename);
  if (e && !*tail)
    {
      *entry = e;
      return SVN_NO_ERROR;
    }
  else if (!e)
    {
      *entry = NULL;
      return SVN_NO_ERROR;
    }

  switch (git_tree_entry_type(e))
    {
      case GIT_OBJ_TREE:
        {
          git_object *obj;
          git_tree *sub_tree;

          SVN_ERR(svn_git__tree_entry_to_object(&obj, tree, e, result_pool));

          sub_tree = (git_tree*)obj;

          SVN_ERR(find_tree_entry(entry, sub_tree, tail,
                                  result_pool, scratch_pool));
          break;
        }
      case GIT_OBJ_BLOB:
        *entry = NULL;
        break;
      default:
        SVN_ERR_MALFUNCTION();
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
find_branch(const git_commit **commit, const char **relpath,
            svn_fs_root_t *root, const char *path, apr_pool_t *pool)
{
  svn_fs_git_root_t *fgr = root->fsap_data;
  const char *branch_path;
  const git_oid *oid;

  if (*path == '/')
    path++;

  if (fgr->rev_path)
    {
      const char *rp = svn_relpath_skip_ancestor(fgr->rev_path, path);

      if (rp)
        {
          *relpath = rp;
          *commit = fgr->commit;
          return SVN_NO_ERROR;
        }
    }

  if (apr_hash_count(fgr->branch_map))
    {
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(pool, fgr->branch_map);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *rp;

          branch_path = apr_hash_this_key(hi);
          rp = svn_relpath_skip_ancestor(branch_path, path);
          if (rp)
            {
              *relpath = rp;
              *commit = apr_hash_this_val(hi);
              return SVN_NO_ERROR;
            }
        }
    }

  SVN_ERR(svn_fs_git__db_find_branch(&branch_path, &oid, NULL,
                                     root->fs, path, root->rev,
                                     pool, pool));

  if (branch_path && oid)
    {
      apr_pool_t *result_pool = apr_hash_pool_get(fgr->branch_map);

      SVN_ERR(find_commit(commit, root, oid, result_pool));

      if (*commit)
        {
          svn_hash_sets(fgr->branch_map,
                        apr_pstrdup(result_pool, branch_path),
                        *commit);

          *relpath = svn_relpath_skip_ancestor(branch_path, path);
          return SVN_NO_ERROR;
        }
    }

  *commit = NULL;
  *relpath = NULL;
  return SVN_NO_ERROR;
}

static svn_error_t *
walk_tree_for_changes(apr_hash_t *changed_paths, const char *relpath,
                      svn_fs_root_t *root,
                      const git_tree *new_tree, const git_tree *old_tree,
                      apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  apr_size_t i;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  for (i = 0; i < git_tree_entrycount(new_tree); i++)
    {
      const git_tree_entry *entry = git_tree_entry_byindex(new_tree, i);
      const char *name = git_tree_entry_name(entry);
      const git_tree_entry *old_entry = NULL;

      svn_pool_clear(iterpool);

      if (old_tree)
        old_entry = git_tree_entry_byname(old_tree, name);

      if (old_entry
          && !git_oid_cmp(git_tree_entry_id(entry),
                          git_tree_entry_id(old_entry))
          && (git_tree_entry_filemode(entry)
                          == git_tree_entry_filemode(old_entry)))
        {
          /* Nothing changed in this subtree */
        }
      else if (old_entry
                && git_tree_entry_type(entry) == GIT_OBJ_TREE
                && git_tree_entry_type(old_entry) == GIT_OBJ_TREE)
        {
          git_object *e_new_tree, *e_old_tree;

          SVN_ERR(svn_git__tree_entry_to_object(&e_new_tree, new_tree,
                                                entry, iterpool));
          SVN_ERR(svn_git__tree_entry_to_object(&e_old_tree, old_tree,
                                                old_entry, iterpool));

          SVN_ERR(walk_tree_for_changes(
                      changed_paths,
                      svn_relpath_join(relpath, name, iterpool), root,
                      (git_tree *)e_new_tree, (git_tree *)e_old_tree,
                      result_pool, scratch_pool));
        }
      else if (old_entry
                && git_tree_entry_type(entry) == GIT_OBJ_BLOB
                && git_tree_entry_type(old_entry) == GIT_OBJ_BLOB)
        {
          svn_fs_path_change2_t *ch;
          const char *epath = svn_relpath_join(relpath, name, iterpool);

          ch = svn_fs__path_change_create_internal(
                    make_id(root, epath, result_pool),
                    svn_fs_path_change_modify,
                    result_pool);
          ch->node_kind = svn_node_file;
          ch->text_mod = TRUE;
          svn_hash_sets(changed_paths, make_fspath(epath, result_pool),
                        ch);
        }
      else if (git_tree_entry_type(entry) == GIT_OBJ_BLOB)
        {
          svn_fs_path_change2_t *ch;
          const char *epath = svn_relpath_join(relpath, name, iterpool);

          ch = svn_fs__path_change_create_internal(
                        make_id(root, epath, result_pool),
                        old_entry ? svn_fs_path_change_replace
                                  : svn_fs_path_change_add,
                        result_pool);
          ch->node_kind = svn_node_file;
          svn_hash_sets(changed_paths, make_fspath(epath, result_pool),
                        ch);
        }
      else if (git_tree_entry_type(entry) == GIT_OBJ_TREE)
        {
          svn_fs_path_change2_t *ch;
          git_object *e_new_tree;
          const char *epath = svn_relpath_join(relpath, name, iterpool);

          ch = svn_fs__path_change_create_internal(
            make_id(root, epath, result_pool),
            old_entry ? svn_fs_path_change_replace
                      : svn_fs_path_change_add,
            result_pool);
          ch->node_kind = svn_node_dir;
          svn_hash_sets(changed_paths, make_fspath(epath, result_pool),
                        ch);

          SVN_ERR(svn_git__tree_entry_to_object(&e_new_tree, new_tree, entry,
                                                iterpool));

          SVN_ERR(walk_tree_for_changes(
                          changed_paths, epath, root,
                          (git_tree *)e_new_tree, NULL,
                          result_pool, scratch_pool));
        }
    }

  if (old_tree)
    for (i = 0; i < git_tree_entrycount(old_tree); i++)
      {
        const git_tree_entry *entry = git_tree_entry_byindex(old_tree, i);
        const char *name = git_tree_entry_name(entry);
        const git_tree_entry *new_entry = git_tree_entry_byname(new_tree, name);
        svn_fs_path_change2_t *ch;
        const char *epath;

        if (new_entry)
          continue; /* Handled in first loop */

        epath = svn_relpath_join(relpath, name, iterpool);

        ch = svn_fs__path_change_create_internal(
                        make_id(root, epath, result_pool),
                        svn_fs_path_change_delete,
                        result_pool);

        switch (git_tree_entry_type(entry))
          {
            case GIT_OBJ_TREE:
              ch->node_kind = svn_node_dir;
              break;
            case GIT_OBJ_BLOB:
              ch->node_kind = svn_node_file;
              break;
            default:
              continue;
          }
        svn_hash_sets(changed_paths, make_fspath(epath, result_pool),
                      ch);
      }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

 /* Determining what has changed in a root */
static svn_error_t *
fs_git_paths_changed(apr_hash_t **changed_paths_p,
                     svn_fs_root_t *root,
                     apr_pool_t *pool)
{
  apr_hash_t *changed_paths = apr_hash_make(pool);
  const git_oid *cmt_oid;
  const char *branch_path;
  const git_commit *commit, *parent_commit;
  git_tree *tree, *parent_tree;
  const git_oid *parent_oid;
  svn_boolean_t found;

  *changed_paths_p = changed_paths;

  if (root->rev == 0)
    return SVN_NO_ERROR;
  else if (root->rev == 1)
    {
      svn_fs_path_change2_t *ch;

      ch = svn_fs__path_change_create_internal(make_id(root, "trunk", pool),
                                               svn_fs_path_change_add,
                                               pool);
      ch->node_kind = svn_node_dir;
      svn_hash_sets(changed_paths, "/trunk", ch);

      ch = svn_fs__path_change_create_internal(make_id(root, "branches",
                                                       pool),
                                               svn_fs_path_change_add,
                                               pool);
      ch->node_kind = svn_node_dir;
      svn_hash_sets(changed_paths, "/branches", ch);
    }

  /* TODO: Add branch + tag changes */

  SVN_ERR(svn_fs_git__db_fetch_oid(&found, &cmt_oid, &branch_path,
                                   root->fs, root->rev,
                                   pool, pool));

  if (!found || !cmt_oid)
    return SVN_NO_ERROR; /* No actual changes in this revision*/

  SVN_ERR(find_commit(&commit, root, cmt_oid, pool));
  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));
  parent_oid = git_commit_parent_id(commit, 0);

  if (parent_oid)
    {
      SVN_ERR(find_commit(&parent_commit, root, parent_oid, pool));
      SVN_ERR(svn_git__commit_tree(&parent_tree, parent_commit, pool));
    }
  else
    {
      parent_commit = NULL;
      parent_tree = NULL;
    }

  SVN_ERR(walk_tree_for_changes(changed_paths, branch_path, root,
                                tree, parent_tree,
                                pool, pool));

  return SVN_NO_ERROR;
}

/* Generic node operations */
static svn_error_t *
fs_git_check_path(svn_node_kind_t *kind_p, svn_fs_root_t *root,
                  const char *path, apr_pool_t *pool)
{
  const git_commit *commit;
  const char *relpath;
  git_tree *tree;
  git_tree_entry *entry;

  if (*path == '/')
    path++;
  if (!*path)
    {
      *kind_p = svn_node_dir;
      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));
  if (!commit)
    {
      if (!strcmp(path, "branches") || !strcmp(path, "tags"))
        {
          *kind_p = svn_node_dir;
          return SVN_NO_ERROR;
        }

      *kind_p = svn_node_none;
      return SVN_NO_ERROR;
    }

  if (!relpath[0])
    {
      *kind_p = svn_node_dir;
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));
  SVN_ERR(find_tree_entry(&entry, tree, relpath, pool, pool));

  if (!entry)
    *kind_p = svn_node_none;
  else
    {
      switch (git_tree_entry_type(entry))
        {
          case GIT_OBJ_TREE:
            *kind_p = svn_node_dir;
            break;
          case GIT_OBJ_BLOB:
            *kind_p = svn_node_file;
            break;
          default:
            *kind_p = svn_node_none;
            break;
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_history(svn_fs_history_t **history_p,
                    svn_fs_root_t *root, const char *path,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  const git_commit *commit;
  const char *relpath;
  git_tree *tree;
  git_tree_entry *entry;

  if (*path == '/')
    path++;
  if (!*path)
    {
      SVN_ERR(svn_fs_git__make_history_simple(history_p,
                                              root, root->rev, 0, NULL,
                                              result_pool, scratch_pool));
      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&commit, &relpath, root, path, result_pool));
  if (!commit)
    {
      if (root->rev > 0
          && !strcmp(path, "branches") || !strcmp(path, "tags"))
        {
          /* Branches and tags subdirs were created in r1 */
          SVN_ERR(svn_fs_git__make_history_simple(history_p,
                                                  root, root->rev, 1, path,
                                                  result_pool, scratch_pool));
          return SVN_NO_ERROR;
        }

      return SVN_FS__NOT_FOUND(root, path);
    }

  if (!relpath[0])
    {
      SVN_ERR(svn_fs_git__make_history_commit(history_p,
                                              root, commit,
                                              result_pool, scratch_pool));
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_git__commit_tree(&tree, commit, scratch_pool));
  SVN_ERR(find_tree_entry(&entry, tree, relpath, scratch_pool, scratch_pool));

  if (!entry)
    return SVN_FS__NOT_FOUND(root, path);

  SVN_ERR(svn_fs_git__make_history_node(history_p,
                                        root, commit, relpath,
                                        result_pool, scratch_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_id(const svn_fs_id_t **id_p, svn_fs_root_t *root,
               const char *path, apr_pool_t *pool)
{
  if (*path == '/')
    path++;

  *id_p = make_id(root, path, pool);
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_relation(svn_fs_node_relation_t *relation,
                     svn_fs_root_t *root_a, const char *path_a,
                     svn_fs_root_t *root_b, const char *path_b,
                     apr_pool_t *scratch_pool)
{
  const git_commit *commit_a, *commit_b;
  const char *relpath_a, *relpath_b;
  git_tree *tree_a, *tree_b;
  const git_tree_entry *entry_a, *entry_b;

  if (*path_a == '/')
    path_a++;
  if (*path_b == '/')
    path_b++;

  if (!*path_a || !*path_b)
    {
      if (!*path_a && !*path_b)
        {
          if (root_a->rev == root_b->rev)
            *relation = svn_fs_node_unchanged;
          else
            *relation = svn_fs_node_common_ancestor;
        }
      else
        *relation = svn_fs_node_unrelated;
      return SVN_NO_ERROR;
    }
  else if (root_a->rev == root_b->rev && !strcmp(path_a, path_b))
    {
      *relation = svn_fs_node_unchanged;
      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&commit_a, &relpath_a, root_a, path_a, scratch_pool));
  SVN_ERR(find_branch(&commit_b, &relpath_b, root_b, path_b, scratch_pool));

  if (!(commit_a && commit_b))
    {
      if (!commit_a && !commit_b && !strcmp(path_a, path_b))
        *relation = svn_fs_node_common_ancestor;
      else
        *relation = svn_fs_node_unrelated;

      return SVN_NO_ERROR;
    }
  else if ((*relpath_a == '\0') || (*relpath_b == '\0'))
    {
      /* trunk, tags/* and branches/* are related */
      if ((*relpath_a == '\0') && (*relpath_b == '\0'))
        *relation = svn_fs_node_common_ancestor;
      else
        *relation = svn_fs_node_unrelated;

      return SVN_NO_ERROR;
    }

  if (strcmp(relpath_a, relpath_b))
    {
      *relation = svn_fs_node_unrelated;
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_git__commit_tree(&tree_a, commit_a, scratch_pool));
  SVN_ERR(svn_git__commit_tree(&tree_b, commit_b, scratch_pool));

  SVN_ERR(find_tree_entry(&entry_a, tree_a, relpath_a,
                          scratch_pool, scratch_pool));
  SVN_ERR(find_tree_entry(&entry_b, tree_b, relpath_b,
                          scratch_pool, scratch_pool));

  if (!entry_a || !entry_b)
    {
      *relation = svn_fs_node_unrelated;
      return SVN_NO_ERROR;
    }

  if (git_tree_entry_type(entry_a) != git_tree_entry_type(entry_b))
    *relation = svn_fs_node_unrelated;
  else if (!git_oid_cmp(git_tree_entry_id(entry_a),
                        git_tree_entry_id(entry_b)))
    {
      *relation = svn_fs_node_unchanged;
    }
  else 
    *relation = svn_fs_node_common_ancestor;

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_created_rev(svn_revnum_t *revision,
                        svn_fs_root_t *root, const char *path,
                        apr_pool_t *pool)
{
  svn_fs_git_root_t *fgr = root->fsap_data;
  const git_commit *rev_commit;
  const char *relpath;
  git_tree *rev_tree;
  git_tree_entry *rev_entry;
  apr_pool_t *iterpool;
  git_oid oid, last_oid;
  const git_oid *oid_p;

  if (*path == '/')
    path++;

  if (*path == '\0')
    {
      *revision = root->rev;
      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&rev_commit, &relpath, root, path, pool));
  if (!rev_commit)
    {
      /* Handle 'branches' and 'tags' dirs */
      *revision = root->rev;
      return SVN_NO_ERROR;
    }
  else if (!*relpath && fgr->rev_path && !strcmp(fgr->rev_path, path))
    {
      /* The root of what has committed, changed... */
      *revision = root->rev;
      return SVN_NO_ERROR;
    }
  iterpool = svn_pool_create(pool);

  SVN_ERR(svn_git__commit_tree(&rev_tree, rev_commit, iterpool));
  SVN_ERR(find_tree_entry(&rev_entry, rev_tree, relpath, pool, iterpool));

  last_oid = *git_commit_id(rev_commit);
  oid_p = git_commit_parent_id(rev_commit, 0);


  while (oid_p)
    {
      const git_commit *cmt;
      git_tree *cmt_tree;
      git_tree_entry *cmt_entry;

      svn_pool_clear(iterpool);

      SVN_ERR(find_commit(&cmt, root, oid_p, iterpool));

      SVN_ERR(svn_git__commit_tree(&cmt_tree, cmt, iterpool));

      SVN_ERR(find_tree_entry(&cmt_entry, cmt_tree, relpath,
                              iterpool, iterpool));

      if (!cmt_entry || git_oid_cmp(git_tree_entry_id(rev_entry),
                                    git_tree_entry_id(cmt_entry)))
        {
          break;
        }

      git_commit_message(cmt);

      last_oid = *git_commit_id(cmt);

      oid_p = git_commit_parent_id(cmt, 0);
      if (!oid_p)
        break;

      /* And prepare for pool cleanup */
      oid = *oid_p;
      oid_p = &oid;
    }

  SVN_ERR(svn_fs_git__db_fetch_rev(revision, NULL,
                                   root->fs, &last_oid,
                                   pool, iterpool));

  if (!SVN_IS_VALID_REVNUM(*revision))
    *revision = root->rev;

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_origin_rev(svn_revnum_t *revision,
                       svn_fs_root_t *root, const char *path,
                       apr_pool_t *pool)
{
  *revision = root->rev; /* No common ancestry */
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_created_path(const char **created_path,
                         svn_fs_root_t *root, const char *path,
                         apr_pool_t *pool)
{
  svn_revnum_t rev;
  git_commit *commit;
  const char *relpath, *basepath;

  SVN_ERR(fs_git_node_created_rev(&rev, root, path, pool));

  if (rev == root->rev)
    {
      *created_path = make_fspath(path, pool);
      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));
  SVN_ERR(svn_fs_git__db_fetch_oid(NULL, NULL, &basepath, root->fs, rev,
                                   pool, pool));

  *created_path = make_fspath(svn_relpath_join(basepath, relpath, pool), pool);
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_delete_node(svn_fs_root_t *root, const char *path,
                   apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_copy(svn_fs_root_t *from_root, const char *from_path,
            svn_fs_root_t *to_root, const char *to_path,
            apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_revision_link(svn_fs_root_t *from_root,
                     svn_fs_root_t *to_root,
                     const char *path,
                     apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_copied_from(svn_revnum_t *rev_p, const char **path_p,
                   svn_fs_root_t *root, const char *path,
                   apr_pool_t *pool)
{
  *rev_p = SVN_INVALID_REVNUM;
  *path_p = NULL;

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_closest_copy(svn_fs_root_t **root_p, const char **path_p,
                    svn_fs_root_t *root, const char *path,
                    apr_pool_t *pool)
{
  *root_p = NULL;
  *path_p = NULL;

  return SVN_NO_ERROR;
}

/* Property operations */
static svn_error_t *
fs_git_node_prop(svn_string_t **value_p, svn_fs_root_t *root,
                 const char *path, const char *propname,
                 apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_node_proplist(apr_hash_t **table_p, svn_fs_root_t *root,
                     const char *path, apr_pool_t *pool)
{
  *table_p = apr_hash_make(pool);

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_node_has_props(svn_boolean_t *has_props, svn_fs_root_t *root,
                      const char *path, apr_pool_t *scratch_pool)
{
  *has_props = FALSE;
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_change_node_prop(svn_fs_root_t *root, const char *path,
                        const char *name,
                        const svn_string_t *value,
                        apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_props_changed(int *changed_p, svn_fs_root_t *root1,
                     const char *path1, svn_fs_root_t *root2,
                     const char *path2, svn_boolean_t strict,
                     apr_pool_t *scratch_pool)
{
  *changed_p = FALSE;
  return SVN_NO_ERROR;
}

/* Directories */
static svn_error_t *
fs_git_dir_entries(apr_hash_t **entries_p, svn_fs_root_t *root,
                   const char *path, apr_pool_t *pool)
{
  svn_fs_dirent_t *de;
  const git_commit *commit;
  git_tree *tree;
  const char *relpath;
  apr_size_t idx;

  if (*path == '/')
    path++;

  *entries_p = apr_hash_make(pool);
  if (!root->rev)
    return SVN_NO_ERROR;
  else if (*path == '\0')
    {
      de = apr_pcalloc(pool, sizeof(*de));
      de->kind = svn_node_dir;
      de->id = make_id(root, path, pool);
      de->name = "trunk";
      svn_hash_sets(*entries_p, "trunk", de);

      de = apr_pcalloc(pool, sizeof(*de));
      de->kind = svn_node_dir;
      de->id = make_id(root, path, pool);
      de->name = "branches";
      svn_hash_sets(*entries_p, "branches", de);

      de = apr_pcalloc(pool, sizeof(*de));
      de->kind = svn_node_dir;
      de->id = make_id(root, path, pool);
      de->name = "tags";
      svn_hash_sets(*entries_p, "tags", de);

      return SVN_NO_ERROR;
    }

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));
  if (!commit)
    {
      apr_hash_t *tags, *branches;
      apr_hash_index_t *hi;

      SVN_ERR(svn_fs_git__db_get_tags_branches(&tags, &branches,
                                               root->fs, path, root->rev,
                                               pool, pool));

      branches = apr_hash_overlay(pool, branches, tags);
      for (hi = apr_hash_first(pool, branches); hi; hi = apr_hash_next(hi))
        {
          const char *name = apr_hash_this_key(hi);
          const char *itempath = apr_hash_this_val(hi);

          de = apr_pcalloc(pool, sizeof(*de));
          de->kind = svn_node_dir;
          de->id = make_id(root, itempath, pool);
          de->name = name;
          svn_hash_sets(*entries_p, name, de);
        }

      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));

  if (*relpath)
    {
      const git_tree_entry *entry;
      git_object *obj;

      SVN_ERR(find_tree_entry(&entry, tree, relpath, pool, pool));

      if (!entry || git_tree_entry_type(entry) != GIT_OBJ_TREE)
        return SVN_FS__ERR_NOT_DIRECTORY(root->fs, path);

      SVN_ERR(svn_git__tree_entry_to_object(&obj, tree, entry, pool));

      tree = (git_tree*)obj;
    }

  for (idx = 0; idx < git_tree_entrycount(tree); idx++)
    {
      const git_tree_entry *e = git_tree_entry_byindex(tree, idx);

      de = apr_pcalloc(pool, sizeof(*de));
      de->name = git_tree_entry_name(e);
      de->id = make_id(root, svn_relpath_join(path, de->name, pool),
                       pool);

      if (git_tree_entry_type(e) == GIT_OBJ_TREE)
        de->kind = svn_node_dir;
      else if (git_tree_entry_type(e) == GIT_OBJ_BLOB)
        de->kind = svn_node_file;
      else
        continue;

      svn_hash_sets(*entries_p, de->name, de);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_dir_optimal_order(apr_array_header_t **ordered_p,
                         svn_fs_root_t *root,
                         apr_hash_t *entries,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  /* 1:1 copy of entries with no difference in ordering */
  apr_hash_index_t *hi;
  apr_array_header_t *result
    = apr_array_make(result_pool, apr_hash_count(entries),
                     sizeof(svn_fs_dirent_t *));
  for (hi = apr_hash_first(scratch_pool, entries); hi; hi = apr_hash_next(hi))
    APR_ARRAY_PUSH(result, svn_fs_dirent_t *) = apr_hash_this_val(hi);

  *ordered_p = result;
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_make_dir(svn_fs_root_t *root, const char *path,
                apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

/* Files */
static svn_error_t *
fs_git_file_length(svn_filesize_t *length_p, svn_fs_root_t *root,
                   const char *path, apr_pool_t *pool)
{
  const git_commit *commit;
  git_tree *tree;
  git_tree_entry *entry;
  git_object *obj;
  git_blob *blob;
  const char *relpath;

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));

  if (!commit)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));

  SVN_ERR(find_tree_entry(&entry, tree, relpath, pool, pool));

  if (!entry || git_tree_entry_type(entry) != GIT_OBJ_BLOB)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  if ((git_tree_entry_filemode(entry) & GIT_FILEMODE_LINK)
      == GIT_FILEMODE_LINK)
    {
      /* ### TODO */
    }

  SVN_ERR(svn_git__tree_entry_to_object(&obj, tree, entry, pool));

  blob = (git_blob*)obj;

  *length_p = git_blob_rawsize(blob);
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_file_checksum(svn_checksum_t **checksum,
                     svn_checksum_kind_t kind, svn_fs_root_t *root,
                     const char *path, apr_pool_t *pool)
{
  const git_commit *commit;
  git_tree *tree;
  git_tree_entry *entry;
  const char *relpath;

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));

  if (!commit)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));

  SVN_ERR(find_tree_entry(&entry, tree, relpath, pool, pool));

  if (!entry || git_tree_entry_type(entry) != GIT_OBJ_BLOB)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  if ((git_tree_entry_filemode(entry) & GIT_FILEMODE_LINK)
      == GIT_FILEMODE_LINK)
    {
      /* ### TODO */
    }

  SVN_ERR(svn_fs_git__db_fetch_checksum(checksum, root->fs,
                                        git_tree_entry_id(entry),
                                        kind, pool, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_file_contents(svn_stream_t **contents,
                     svn_fs_root_t *root, const char *path,
                     apr_pool_t *pool)
{
  const git_commit *commit;
  git_tree *tree;
  git_tree_entry *entry;
  const char *relpath;

  SVN_ERR(find_branch(&commit, &relpath, root, path, pool));

  if (!commit)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  SVN_ERR(svn_git__commit_tree(&tree, commit, pool));

  SVN_ERR(find_tree_entry(&entry, tree, relpath, pool, pool));

  if (!entry || git_tree_entry_type(entry) != GIT_OBJ_BLOB)
    return SVN_FS__ERR_NOT_FILE(root->fs, path);

  if ((git_tree_entry_filemode(entry) & GIT_FILEMODE_LINK)
      == GIT_FILEMODE_LINK)
    {
      /* ### TODO */
    }

  SVN_ERR(svn_fs_git__get_blob_stream(contents, root->fs,
                                      git_tree_entry_id(entry),
                                      pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_try_process_file_contents(svn_boolean_t *success,
                                 svn_fs_root_t *target_root,
                                 const char *target_path,
                                 svn_fs_process_contents_func_t processor,
                                 void* baton,
                                 apr_pool_t *pool)
{
  *success = FALSE;

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_make_file(svn_fs_root_t *root, const char *path,
                 apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_apply_textdelta(svn_txdelta_window_handler_t *contents_p,
                       void **contents_baton_p,
                       svn_fs_root_t *root, const char *path,
                       svn_checksum_t *base_checksum,
                       svn_checksum_t *result_checksum,
                       apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_apply_text(svn_stream_t **contents_p, svn_fs_root_t *root,
                  const char *path, svn_checksum_t *result_checksum,
                  apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

static svn_error_t *
fs_git_contents_changed(int *changed_p,
                        svn_fs_root_t *root_a, const char *path_a,
                        svn_fs_root_t *root_b, const char *path_b,
                        svn_boolean_t strict,
                        apr_pool_t *scratch_pool)
{
  const git_commit *commit_a, *commit_b;
  const char *relpath_a, *relpath_b;
  git_tree *tree_a, *tree_b;
  const git_tree_entry *entry_a, *entry_b;

  SVN_ERR(find_branch(&commit_a, &relpath_a, root_a, path_a, scratch_pool));
  SVN_ERR(find_branch(&commit_b, &relpath_b, root_b, path_b, scratch_pool));

  if (!commit_a)
    return SVN_FS__ERR_NOT_FILE(root_a->fs, path_a);
  else if (!commit_b)
    return SVN_FS__ERR_NOT_FILE(root_b->fs, path_b);

  SVN_ERR(svn_git__commit_tree(&tree_a, commit_a, scratch_pool));
  SVN_ERR(svn_git__commit_tree(&tree_b, commit_b, scratch_pool));

  SVN_ERR(find_tree_entry(&entry_a, tree_a, relpath_a,
                          scratch_pool, scratch_pool));
  SVN_ERR(find_tree_entry(&entry_b, tree_b, relpath_b,
                          scratch_pool, scratch_pool));

  if (!entry_a)
    return SVN_FS__ERR_NOT_FILE(root_a->fs, path_a);
  else if (!entry_b)
    return SVN_FS__ERR_NOT_FILE(root_b->fs, path_b);

  if (!git_oid_cmp(git_tree_entry_id(entry_a),
                   git_tree_entry_id(entry_b)))
    {
      *changed_p = FALSE;
    }
  else
    *changed_p = TRUE;

  return SVN_NO_ERROR;
}

static svn_error_t *
fs_git_get_file_delta_stream(svn_txdelta_stream_t **stream_p,
                             svn_fs_root_t *source_root,
                             const char *source_path,
                             svn_fs_root_t *target_root,
                             const char *target_path,
                             apr_pool_t *pool)
{
  svn_stream_t *source, *target;
  svn_txdelta_stream_t *delta_stream;

  /* Get read functions for the source file contents.  */
  if (source_root && source_path)
    SVN_ERR(fs_git_file_contents(&source, source_root, source_path, pool));
  else
    source = svn_stream_empty(pool);

  /* Get read functions for the target file contents.  */
  SVN_ERR(fs_git_file_contents(&target, target_root, target_path, pool));

  /* Create a delta stream that turns the ancestor into the target.  */
  svn_txdelta2(&delta_stream, source, target, TRUE, pool);

  *stream_p = delta_stream;
  return SVN_NO_ERROR;
}

/* Merging. */
static svn_error_t *
fs_git_merge(const char **conflict_p,
             svn_fs_root_t *source_root,
             const char *source_path,
             svn_fs_root_t *target_root,
             const char *target_path,
             svn_fs_root_t *ancestor_root,
             const char *ancestor_path,
             apr_pool_t *pool)
{
  return svn_error_create(APR_ENOTIMPL, NULL, NULL);
}

/* Mergeinfo. */
static svn_error_t *
fs_git_get_mergeinfo(svn_mergeinfo_catalog_t *catalog,
                     svn_fs_root_t *root,
                     const apr_array_header_t *paths,
                     svn_mergeinfo_inheritance_t inherit,
                     svn_boolean_t include_descendants,
                     svn_boolean_t adjust_inherited_mergeinfo,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  return svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL, NULL);
}

static const root_vtable_t root_vtable =
{
  fs_git_paths_changed,
  fs_git_check_path,
  fs_git_node_history,
  fs_git_node_id,
  fs_git_node_relation,
  fs_git_node_created_rev,
  fs_git_node_origin_rev,
  fs_git_node_created_path,
  fs_git_delete_node,
  fs_git_copy,
  fs_git_revision_link,
  fs_git_copied_from,
  fs_git_closest_copy,
  fs_git_node_prop,
  fs_git_node_proplist,
  fs_git_node_has_props,
  fs_git_change_node_prop,
  fs_git_props_changed,
  fs_git_dir_entries,
  fs_git_dir_optimal_order,
  fs_git_make_dir,
  fs_git_file_length,
  fs_git_file_checksum,
  fs_git_file_contents,
  fs_git_try_process_file_contents,
  fs_git_make_file,
  fs_git_apply_textdelta,
  fs_git_apply_text,
  fs_git_contents_changed,
  fs_git_get_file_delta_stream,
  fs_git_merge,
  fs_git_get_mergeinfo
};

svn_error_t *
svn_fs_git__revision_root(svn_fs_root_t **root_p, svn_fs_t *fs, svn_revnum_t rev, apr_pool_t *pool)
{
  svn_fs_root_t *root;
  svn_fs_git_root_t *fgr;

  SVN_ERR(svn_fs__check_fs(fs, TRUE));


  fgr = apr_pcalloc(pool, sizeof(*fgr));
  root = apr_pcalloc(pool, sizeof(*root));

  root->pool = pool;
  root->fs = fs;
  root->is_txn_root = FALSE;
  root->txn = NULL;
  root->txn_flags = 0;
  root->rev = rev;

  root->vtable = &root_vtable;
  root->fsap_data = fgr;

  fgr->branch_map = apr_hash_make(pool);

  if (rev > 0)
    {
      git_oid *oid;
      SVN_ERR(svn_fs_git__db_fetch_oid(&fgr->exact, &oid, &fgr->rev_path,
                                       fs, rev, pool, pool));

      if (oid)
        SVN_ERR(find_commit(&fgr->commit, root, oid, pool));
    }

  *root_p = root;

  return SVN_NO_ERROR;
}
