/*
 * ra_dav.h :  private declarations for the RA/DAV module
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */



#ifndef RA_DAV_H
#define RA_DAV_H

#include <apr_pools.h>
#include <apr_tables.h>

#include <ne_request.h>
#include <ne_uri.h>
#include <ne_207.h>            /* for ne_propname, NE_ELM_207_UNUSED */

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_delta.h"
#include "svn_ra.h"


typedef struct {
  apr_pool_t *pool;

  struct uri root;              /* repository root */
  const char *username;
  const char *password;

  ne_session *sess;           /* HTTP session to server */

} svn_ra_session_t;


#ifdef SVN_DEBUG
#define DEBUG_CR "\n"
#else
#define DEBUG_CR ""
#endif


/** plugin function prototypes */

svn_error_t *svn_ra_dav__get_latest_revnum(void *session_baton,
                                           svn_revnum_t *latest_revnum);

svn_error_t *svn_ra_dav__get_dated_revision (void *session_baton,
                                             svn_revnum_t *revision,
                                             apr_time_t time);

svn_error_t * svn_ra_dav__get_commit_editor(
  void *session_baton,
  const svn_delta_edit_fns_t **editor,
  void **edit_baton,
  svn_stringbuf_t *log_msg,
  svn_ra_get_wc_prop_func_t get_func,
  svn_ra_set_wc_prop_func_t set_func,
  svn_ra_close_commit_func_t close_func,
  void *close_baton);

svn_error_t * svn_ra_dav__abort_commit(
 void *session_baton,
 void *edit_baton);

svn_error_t * svn_ra_dav__do_checkout (
  void *session_baton,
  svn_revnum_t revision,
  const svn_delta_edit_fns_t *editor,
  void *edit_baton);

svn_error_t * svn_ra_dav__do_update(
  void *session_baton,
  const svn_ra_reporter_t **reporter,
  void **report_baton,
  svn_revnum_t revision_to_update_to,
  const svn_delta_edit_fns_t *wc_update,
  void *wc_update_baton);

/*
** SVN_RA_DAV__LP_*: local properties for RA/DAV
**
** ra_dav stores properties on the client containing information needed
** to operate against the SVN server. Some of this informations is strictly
** necessary to store, and some is simply stored as a cached value.
*/

#define SVN_RA_DAV__LP_NAMESPACE \
        "svn:wc:http://subversion.tigris.org/props/ra/dav/local/"

#define SVN_RA_DAV__CUSTOM_NAMESPACE \
	"SVN:custom:"

/* store the URL where Activities can be created */
#define SVN_RA_DAV__LP_ACTIVITY_URL     SVN_RA_DAV__LP_NAMESPACE "activity-url"

/* store the URL of the version resource (from the DAV:checked-in property) */
#define SVN_RA_DAV__LP_VSN_URL          SVN_RA_DAV__LP_NAMESPACE "version-url"


/*
** SVN_RA_DAV__PROP_*: properties that we fetch from the server
**
** These are simply symbolic names for some standard properties that we fetch.
*/
#define SVN_RA_DAV__PROP_BASELINE_COLLECTION    "DAV:baseline-collection"
#define SVN_RA_DAV__PROP_CHECKED_IN     "DAV:checked-in"
#define SVN_RA_DAV__PROP_VCC            "DAV:version-controlled-configuration"
#define SVN_RA_DAV__PROP_VERSION_NAME   "DAV:version-name"

#define SVN_RA_DAV__PROP_BASELINE_RELPATH       "SVN:baseline-relative-path"

typedef struct {
  /* what is the URL for this resource */
  const char *url;

  /* is this resource a collection? (from the DAV:resourcetype element) */
  int is_collection;

  /* PROPSET: NAME -> VALUE (const char * -> const char *) */
  apr_hash_t *propset;

  /* --- only used during response processing --- */
  /* when we see a DAV:href element, what element is the parent? */
  int href_parent;

  apr_pool_t *pool;

} svn_ra_dav_resource_t;

/* ### WARNING: which_props can only identify properties which props.c
   ### knows about. see the elem_definitions[] array. */

/* fetch a bunch of properties from the server. */
svn_error_t * svn_ra_dav__get_props(apr_hash_t **results,
                                    svn_ra_session_t *ras,
                                    const char *url,
                                    int depth,
                                    const char *label,
                                    const ne_propname *which_props,
                                    apr_pool_t *pool);

/* fetch a single resource's props from the server. */
svn_error_t * svn_ra_dav__get_props_resource(svn_ra_dav_resource_t **rsrc,
                                             svn_ra_session_t *ras,
                                             const char *url,
                                             const char *label,
                                             const ne_propname *which_props,
                                             apr_pool_t *pool);

/* fetch a single property from a single resource */
svn_error_t * svn_ra_dav__get_one_prop(const svn_string_t **propval,
                                       svn_ra_session_t *ras,
                                       const char *url,
                                       const char *label,
                                       const ne_propname *propname,
                                       apr_pool_t *pool);

extern const ne_propname svn_ra_dav__vcc_prop;
extern const ne_propname svn_ra_dav__checked_in_prop;




/* send an OPTIONS request to fetch the activity-collection-set */
svn_error_t * svn_ra_dav__get_activity_url(svn_stringbuf_t **activity_url,
                                           svn_ra_session_t *ras,
                                           const char *url,
                                           apr_pool_t *pool);

svn_error_t *svn_ra_dav__parsed_request(svn_ra_session_t *ras,
                                        const char *method,
                                        const char *url,
                                        const char *body,
                                        int fd,
                                        const struct ne_xml_elm *elements, 
                                        ne_xml_validate_cb validate_cb,
                                        ne_xml_startelm_cb startelm_cb, 
                                        ne_xml_endelm_cb endelm_cb,
                                        void *baton,
                                        apr_pool_t *pool);

/* ### add SVN_RA_DAV_ to these to prefix conflicts with (sys) headers? */
enum {
  /* DAV elements */
  ELEM_activity_coll_set = NE_ELM_207_UNUSED,
  ELEM_add_directory,
  ELEM_add_file,
  ELEM_baseline,
  ELEM_baseline_coll,
  ELEM_checked_in,
  ELEM_collection,
  ELEM_delete_entry,
  ELEM_fetch_file,
  ELEM_fetch_props,
  ELEM_ignored_set,
  ELEM_merge_response,
  ELEM_merged_set,
  ELEM_options_response,
  ELEM_replace_directory,
  ELEM_replace_file,
  ELEM_resourcetype,
  ELEM_target_revision,
  ELEM_update_report,
  ELEM_updated_set,
  ELEM_vcc,
  ELEM_version_name,

  /* SVN elements */
  ELEM_baseline_relpath
};

/* ### docco */
svn_error_t * svn_ra_dav__merge_activity(
    svn_ra_session_t *ras,
    const char *repos_url,
    const char *activity_url,
    svn_ra_set_wc_prop_func_t set_prop,
    svn_ra_close_commit_func_t close_commit,
    void *close_baton,
    apr_array_header_t *deleted_entries,
    apr_pool_t *pool);


/* Make a buffer for repeated use with svn_stringbuf_set().
   ### it would be nice to start this buffer with N bytes, but there isn't
   ### really a way to do that in the string interface (yet), short of
   ### initializing it with a fake string (and copying it) */
#define MAKE_BUFFER(p) svn_stringbuf_ncreate("", 0, (p))

void svn_ra_dav__copy_href(svn_stringbuf_t *dst, const char *src);

#endif  /* RA_DAV_H */


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
