/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2012-2023, Karl Heyes <karl@kheyes.plus.com>,
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org>,
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_POLL
#include <sys/poll.h>
#endif

#ifdef _MSC_VER
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
# ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
# endif
# ifndef SCN_OFF_T
#  define SCN_OFF_T SCNdMAX
# endif
# ifndef PRI_OFF_T
#  define PRI_OFF_T PRIdMAX
# endif
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#include "thread/thread.h"
#include "avl/avl.h"
#include "httpp/httpp.h"
#include "net/sock.h"

#include "fserve.h"
#include "connection.h"
#include "global.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "format.h"
#include "logging.h"
#include "cfgfile.h"
#include "util.h"
#include "admin.h"
#include "slave.h"

#include "format_mp3.h"

#undef CATMODULE
#define CATMODULE "fserve"

#define BUFSIZE 4096

static mutex_t pending_lock;
static avl_tree *mimetypes = NULL;
static avl_tree *fh_cache = NULL;
#ifndef HAVE_PREAD
static mutex_t seekread_lock;
#endif

typedef struct {
    char *ext;
    char *type;
} mime_type;

typedef struct {
    fbinfo finfo;
    mutex_t lock;
    int prev_count;
    int refcount;
    int peak;
    int max;
    icefile_handle f;
    time_t stats_update;
    time_t expire;
    long frame_start_pos;
    stats_handle_t stats;
    format_plugin_t *format;
    struct rate_calc *out_bitrate;
    avl_tree *clients;
} fh_node;

int fserve_running;

static int _delete_mapping(void *mapping);
static int prefile_send (client_t *client);
static int file_send (client_t *client);
static int _compare_fh(void *arg, void *a, void *b);
static int _delete_fh (void *mapping);
static void remove_fh_from_cache (fh_node *fh);

static fh_node no_file;


void fserve_initialize(void)
{
    if (fserve_running) return;

    ice_config_t *config = config_get_config();

    mimetypes = NULL;
    thread_mutex_create (&pending_lock);
#ifndef HAVE_PREAD
    thread_mutex_create (&seekread_lock);
#endif
    fh_cache = avl_tree_new (_compare_fh, NULL);

    fserve_recheck_mime_types (config);
    config_release_config();

    stats_event_flags (NULL, "file_connections", "0", STATS_COUNTERS);
    fserve_running = 1;
    memset (&no_file, 0, sizeof (no_file));
    thread_mutex_create (&no_file.lock);
    no_file.clients = avl_tree_new (client_compare, NULL);
    no_file.refcount = 1;
    no_file.expire = (time_t)-1;
    no_file.f = -1;
    avl_insert (fh_cache, &no_file);
    INFO0("file serving started");
}

void fserve_shutdown(void)
{
    fserve_running = 0;
    if (mimetypes)
        avl_tree_free (mimetypes, _delete_mapping);
    if (fh_cache)
    {
        int count = 20;
        avl_delete (fh_cache, &no_file, NULL);
        while (fh_cache->length > 1 && count)
        {
            fh_node *fh = fh_cache->root->right->key;
            if (fh && fh->refcount == 0)
            {
                remove_fh_from_cache (fh);
                _delete_fh (fh);
                continue;
            }
            DEBUG1 ("waiting for %u entries to clear", fh_cache->length);
            thread_sleep (100000);
            count--;
        }
        avl_tree_free (fh_cache, _delete_fh);
    }

    thread_mutex_destroy (&pending_lock);
#ifndef HAVE_PREAD
    thread_mutex_destroy (&seekread_lock);
#endif
    INFO0("file serving stopped");
}


/* string returned needs to be free'd */
char *fserve_content_type (const char *path)
{
    char *ext = util_get_extension(path);
    mime_type exttype = { NULL, NULL };
    void *result;
    char *type;

    if (ext == NULL)
        return strdup ("text/html");
    exttype.ext = strdup (ext);

    thread_mutex_lock (&pending_lock);
    if (mimetypes && !avl_get_by_key (mimetypes, &exttype, &result))
    {
        mime_type *mime = result;
        type = strdup (mime->type);
    }
    else
        type = strdup ("application/octet-stream");
    thread_mutex_unlock (&pending_lock);
    free (exttype.ext);
    return type;
}


static int _compare_fh(void *arg, void *a, void *b)
{
    fh_node *x = a, *y = b;
    int r = 0;

    if (x->finfo.mount == NULL && y->finfo.mount)
       return -1;
    if (x->finfo.mount && y->finfo.mount == NULL)
       return 1;
    if (x->finfo.mount && y->finfo.mount)
    {
        r = strcmp (x->finfo.mount, y->finfo.mount);
        if (r) return r;
    }
    r = (int)x->finfo.flags - y->finfo.flags;
    return r;
}


static int _delete_fh (void *mapping)
{
    fh_node *fh = mapping;
    if (fh == &no_file)
    {
        ERROR0 ("no file handle free detected");
        return 0;
    }
    if (fh->refcount)
        WARN2 ("handle for %s has refcount %d", fh->finfo.mount, fh->refcount);
    else
        thread_mutex_destroy (&fh->lock);

    file_close (&fh->f);
    if (fh->format)
    {
        free (fh->format->mount);
        format_plugin_clear (fh->format, NULL);
        free (fh->format);
    }
    if (fh->clients)
        avl_tree_free (fh->clients, NULL);
    rate_free (fh->out_bitrate);
    free (fh->finfo.mount);
    free (fh->finfo.override);
    free (fh);

    return 1;
}


static void remove_fh_from_cache (fh_node *fh)
{
    if (fh->refcount)
        WARN2 ("removing %s with %d still on", fh->finfo.mount, fh->refcount);
    avl_delete (fh_cache, fh, NULL);
}


static void fh_stats (fh_node *fh, int enable)
{
    if (enable)
    {
        if (fh->finfo.limit == 0) return; // stats only appear for rate limited files
        if (fh->stats == 0)
        {
            int len = strlen (fh->finfo.mount) + 15;
            char buf [len];
            snprintf (buf, len, "%s-%s", (fh->finfo.flags & FS_FALLBACK) ? "fallback" : "file", fh->finfo.mount);
            fh->stats = stats_handle (buf);
            fh->prev_count = ~fh->refcount; // trigger update of listeners
        }
        else
            stats_lock (fh->stats, NULL);
        if (fh->finfo.flags & FS_FALLBACK)
            stats_set_flags (fh->stats, "fallback", "file", STATS_COUNTERS|STATS_HIDDEN);
        stats_set_flags (fh->stats, "outgoing_kbitrate", "0", STATS_COUNTERS|STATS_HIDDEN);
        stats_release (fh->stats);
    }
    else if (fh->stats)
    {   // drop the stats if enable == 0
        stats_lock (fh->stats, NULL);
        stats_set (fh->stats, NULL, NULL);
        fh->stats = 0;
    }
}


static void remove_from_fh (fh_node *fh, client_t *client)
{
    thread_mutex_lock (&fh->lock);
    fh->refcount--;
    if (fh->clients)
    {
        avl_delete (fh->clients, client, NULL);
        if ((fh->refcount != fh->clients->length && fh->finfo.mount) || ((fh->refcount != fh->clients->length+1) && fh->finfo.mount == NULL))
            ERROR3 (" on %s, with ref %d, len %d", fh->finfo.mount, fh->refcount, fh->clients->length);
    }
    if (fh->refcount == 0 && fh->finfo.mount)
    {
        rate_free (fh->out_bitrate);
        if (fh->finfo.flags & FS_FALLBACK)
            fh_stats (fh, 0);
        else
        {
            fh->out_bitrate = NULL;
            if (fh->finfo.flags & FS_DELETE)
            {
                thread_mutex_unlock (&fh->lock);
                _delete_fh (fh);
                return;
            }
            DEBUG1 ("setting timeout as no clients on %s", fh->finfo.mount);
            fh->expire = time(NULL) + 120;
        }
        fh->out_bitrate = rate_setup (10000, 1000);
    }
    thread_mutex_unlock (&fh->lock);
}


static fh_node *find_fh (fbinfo *finfo)
{
    char *s = finfo->mount;
    fh_node fh, *result = NULL;
    if (finfo->mount == NULL)
    {
        ERROR0 ("missing name");
        return NULL;
    }
    memcpy (&fh.finfo, finfo, sizeof (fbinfo));
    if (strncmp (s, "fallback-", 9) == 0)
    {
        fh.finfo.flags |= FS_FALLBACK;
        fh.finfo.mount = s+9;
    }
    else if (strncmp (s, "file-", 5) == 0)
        fh.finfo.mount = s+5;
    if (avl_get_by_key (fh_cache, &fh, (void**)&result) == 0)
    {
        DEBUG2 ("mount %s (%d)", finfo->mount, finfo->flags);
        return result;
    }
    DEBUG2 ("%s (%d) not found in cache", finfo->mount, finfo->flags);
    return NULL;
}


static void fh_add_client (fh_node *fh, client_t *client)
{
    if (fh->clients == NULL)
        return;
    avl_insert (fh->clients, client);
    if (fh->refcount == 0 && fh->finfo.limit)
        fh_stats (fh, 1);
    fh->refcount++;
    if ((fh->refcount != fh->clients->length && fh->finfo.mount) || ((fh->refcount != fh->clients->length+1) && fh->finfo.mount == NULL))
        ERROR3 (" on %s, with ref %d, len %d", fh->finfo.mount, fh->refcount, fh->clients->length);
    if (fh->refcount > fh->peak)
        fh->peak = fh->refcount;
    if (fh->finfo.mount)
        DEBUG2 ("refcount now %d for %s", fh->refcount, fh->finfo.mount);
}


/* find/create handle and return it with the structure in a locked state */
// requires config read lock and fh_cache write lock on entry. New node is added or
// existing one returned. config used for the path lookup. Both are dropped on exit
//
static fh_node *open_fh (fbinfo *finfo, mount_proxy *minfo)
{
    fh_node *fh, *result;

    if (finfo->mount == NULL)
        finfo->mount = "";
    fh = calloc (1, sizeof (fh_node));
    memcpy (&fh->finfo, finfo, sizeof (fbinfo));
    if (avl_get_by_key (fh_cache, fh, (void**)&result) == 0)
    {
        config_release_config ();
        free (fh);
        thread_mutex_lock (&result->lock);
        avl_tree_unlock (fh_cache);
        if (finfo->flags & FS_FALLBACK)
        {
            if (result->finfo.type != finfo->type && finfo->type != FORMAT_TYPE_UNDEFINED)
            {
                WARN1 ("format mismatched for %s", finfo->mount);
                thread_mutex_unlock (&result->lock);
                return NULL;
            }
        }
        return result;
    }

    // insert new one
    if (fh->finfo.mount[0])
    {
        char *fullpath= util_get_path_from_normalised_uri (fh->finfo.mount, fh->finfo.flags&FS_USE_ADMIN);
        config_release_config ();

        char *contenttype = fserve_content_type (fullpath);
        format_type_t type = format_get_type (contenttype);

        if (fh->finfo.type == FORMAT_TYPE_UNDEFINED)
            fh->finfo.type = type;
        if (finfo->flags & FS_FALLBACK)
        {
            if (fh->finfo.type != type && type != FORMAT_TYPE_UNDEFINED && fh->finfo.type != FORMAT_TYPE_UNDEFINED)
            {
                avl_tree_unlock (fh_cache);
                free (contenttype);
                free (fullpath);
                free (fh);
                WARN1 ("format mismatched for %s", finfo->mount);
                return NULL;
            }
            fh->expire = (time_t)-1;
            INFO2 ("lookup of fallback file \"%s\" (%" PRIu64 ")", finfo->mount, finfo->limit);
        }
        else
            INFO1 ("lookup of \"%s\"", finfo->mount);
        if (file_open (&fh->f, fullpath) < 0)
        {
            INFO1 ("Failed to open \"%s\"", fullpath);
            avl_tree_unlock (fh_cache);
            free (contenttype);
            free (fullpath);
            free (fh);
            return NULL;
        }
        free (fullpath);
        fh->format = calloc (1, sizeof (format_plugin_t));
        fh->format->type = fh->finfo.type;
        fh->format->contenttype = strdup (contenttype);
        free (contenttype);
        if (fh->finfo.type != FORMAT_TYPE_UNDEFINED)
        {
            fh->format->mount = strdup (fh->finfo.mount);
            if (format_get_plugin (fh->format) < 0)
            {
                avl_tree_unlock (fh_cache);
                file_close (&fh->f);
                free (fh->format);
                free (fh);
                return NULL;
            }
            if (fh->format && fh->format->apply_settings)
                fh->format->apply_settings (fh->format, minfo);
            format_check_t fcheck;
            fcheck.fd = fh->f;
            fcheck.desc = finfo->mount;
            if (format_check_frames (&fcheck) < 0 || fcheck.type == FORMAT_TYPE_UNDEFINED)
                WARN1 ("different type detected for %s", finfo->mount);
            else
            {
                fh->frame_start_pos = fcheck.offset;
                if (fh->finfo.limit && fcheck.bitrate > 0)
                {
                    float ratio = (float)fh->finfo.limit / (fcheck.bitrate/8);
                    if (ratio < 0.9 || ratio > 1.1)
                        WARN3 ("bitrate from %s (%u), was expecting %" PRIu64, finfo->mount, (fcheck.bitrate/1000), (fh->finfo.limit/1000*8));
                }
            }
        }
    }
    else
        config_release_config ();
    fh->clients = avl_tree_new (client_compare, NULL);
    if (fh->finfo.limit)
        fh->out_bitrate = rate_setup (10000, 1000);
    thread_mutex_create (&fh->lock);
    thread_mutex_lock (&fh->lock);
    avl_insert (fh_cache, fh);
    avl_tree_unlock (fh_cache);

    fh->refcount = 0;
    fh->peak = 0;
    fh->finfo.mount = strdup (finfo->mount);
    fh->finfo.override = NULL;

    return fh;
}


/* client has requested a file, so check for it and send the file.  Do not
 * refer to the client_t afterwards.  return 0 for success, -1 on error.
 */
int fserve_client_create (client_t *httpclient, const char *path)
{
    struct stat file_buf;
    char *fullpath;
    int m3u_requested = 0, m3u_file_available = 1;
    int xspf_requested = 0, xspf_file_available = 1;
    int ret = -1;
    ice_config_t *config;
    fbinfo finfo;

    config = config_get_config();
    int file_serving = config->fileserve;
    fullpath = util_get_path_from_normalised_uri (path, 0);
    config_release_config();
    DEBUG2 ("checking for file %s (%s)", path, fullpath);

    if (strcmp (util_get_extension (fullpath), "m3u") == 0)
        m3u_requested = 1;

    if (strcmp (util_get_extension (fullpath), "xspf") == 0)
        xspf_requested = 1;

    client_set_queue (httpclient, NULL);

    /* check for the actual file */
    if (stat (fullpath, &file_buf) != 0)
    {
        /* the m3u can be generated, but send an m3u file if available */
        if (m3u_requested == 0 && xspf_requested == 0)
        {
            config = config_get_config();
            if (config->fileserve_redirect == 0 || httpclient->flags & CLIENT_IS_SLAVE ||
                redirect_client (path, httpclient) == 0)
            {
                if ((httpclient->flags & CLIENT_SKIP_ACCESSLOG) == 0)
                    WARN2 ("req for file \"%s\" %s", fullpath, strerror (errno));
                ret = client_send_404 (httpclient, "The file you requested could not be found");
            }
            config_release_config();
            free (fullpath);
            return ret;
        }
        m3u_file_available = 0;
        xspf_file_available = 0;
    }

    if (m3u_requested && m3u_file_available == 0)
    {
        free (fullpath);
        return client_send_m3u (httpclient, path);
    }
    if (xspf_requested && xspf_file_available == 0)
    {
        xmlDocPtr doc;
        char *reference = strdup (path);
        char *eol = strrchr (reference, '.');
        if (eol)
            *eol = '\0';
        doc = stats_get_xml (0, reference);
        free (reference);
        free (fullpath);
        return admin_send_response (doc, httpclient, XSLT, "xspf.xsl");
    }

    /* on demand file serving check */
    if (file_serving == 0)
    {
        DEBUG1 ("on demand file \"%s\" refused", fullpath);
        free (fullpath);
        return client_send_404 (httpclient, "The file you requested could not be found");
    }

    if (S_ISREG (file_buf.st_mode) == 0)
    {
        WARN1 ("found requested file but there is no handler for it: %s", fullpath);
        free (fullpath);
        return client_send_404 (httpclient, "The file you requested could not be found");
    }

    free (fullpath);
    finfo.flags = 0;
    finfo.mount = (char *)path;
    finfo.override = NULL;
    finfo.limit = 0;
    finfo.type = FORMAT_TYPE_UNDEFINED;
    finfo.fsize = file_buf.st_size;

    stats_event_inc (NULL, "file_connections");

    return fserve_setup_client_fb (httpclient, &finfo);
}



static void file_release (client_t *client)
{
    fh_node *fh = client->shared_data;
    int ret = -1;

    if ((fh->finfo.flags & FS_FALLBACK) && (client->flags & CLIENT_AUTHENTICATED))
    {
        // reduce from global count
        global_lock();
        global.listeners--;
        global_unlock();
    }

    client_set_queue (client, NULL);

    if (client->flags & CLIENT_AUTHENTICATED && client->parser->req_type == httpp_req_get)
    {
        const char *m = NULL;
        const char *uri = util_normalise_uri (httpp_getvar (client->parser, HTTPP_VAR_URI));

        if (strcmp (uri, "/admin.cgi") == 0 || strncmp("/admin/", uri, 7) == 0)
            remove_from_fh (fh, client);
        else {
            if (fh->finfo.flags & FS_FALLBACK)
                m = uri;
            else if (client->mount)
                m = client->mount;
            else
                m = fh->finfo.mount;
            if (m)
            {
                ice_config_t *config;
                char *mount = strdup (m);
                mount_proxy *mountinfo;

                remove_from_fh (fh, client);
                client->shared_data = NULL;
                config = config_get_config ();
                mountinfo = config_find_mount (config, mount);
                if (mountinfo && mountinfo->access_log.name)
                    logging_access_id (&mountinfo->access_log, client);
                ret = auth_release_listener (client, mount, mountinfo);
                config_release_config();
                free (mount);
            }
            else
                remove_from_fh (fh, client);
        }
    }
    else
        remove_from_fh (fh, client);
    if (ret < 0)
    {
        client->shared_data = NULL;
        client->flags &= ~CLIENT_AUTHENTICATED;
        client_destroy (client);
    }
    global_reduce_bitrate_sampling (global.out_bitrate);
}


struct _client_functions buffer_content_ops =
{
    prefile_send,
    file_release
};


struct _client_functions file_content_ops =
{
    file_send,
    file_release
};


static int fserve_move_listener (client_t *client)
{
    fh_node *fh = client->shared_data;
    int ret = 0;
    fbinfo f;

    memset (&f, 0, sizeof (f));
    if (client->refbuf && client->pos < client->refbuf->len)
        client->flags |= CLIENT_HAS_INTRO_CONTENT; // treat it as a partial write needing completion
    else
        client_set_queue (client, NULL);
    f.flags = fh->finfo.flags & (~FS_DELETE);
    f.limit = fh->finfo.limit;
    f.mount = fh->finfo.override;
    f.type = fh->finfo.type;
    if (move_listener (client, &f) < 0)
    {
        WARN1 ("moved failed, terminating listener on %s", fh->finfo.mount);
        ret = -1;
    }
    else
    {
        DEBUG3 ("moved %s from %s (%d)", client->connection.ip, fh->finfo.mount, fh->finfo.flags);
        ret = 0;
        remove_from_fh (fh, client);
    }
    return ret;
}


static int fserve_change_worker (client_t *client)
{
    worker_t *this_worker = client->worker, *worker;
    int ret = 0;

    if (this_worker->move_allocations == 0)
        return 0;
    thread_rwlock_rlock (&workers_lock);
    worker = worker_selected ();
    if (worker && worker != client->worker)
    {
        long diff = this_worker->move_allocations < 1000000 ? this_worker->count - worker->count : 1000;
        if (diff > 10)
        {
            this_worker->move_allocations--;
            ret = client_change_worker (client, worker);
        }
    }
    thread_rwlock_unlock (&workers_lock);
    if (ret)
        DEBUG2 ("moving listener from %p to %p", this_worker, worker);
    return ret;
}


struct _client_functions throttled_file_content_ops;

static int prefile_send (client_t *client)
{
    int loop = 8, bytes, written = 0;
    worker_t *worker = client->worker;

    while (loop)
    {
        refbuf_t *refbuf = client->refbuf;
        fh_node *fh = client->shared_data;
        loop--;
        if (fserve_running == 0 || client->connection.error)
            return -1;
        if (refbuf == NULL || client->pos == refbuf->len)
        {
            if (fh->finfo.override && (client->flags & CLIENT_AUTHENTICATED))
                return fserve_move_listener (client);

            if (refbuf == NULL || refbuf->next == NULL)
            {
                if ((client->flags & CLIENT_AUTHENTICATED) == 0)
                    return -1;
                if (file_in_use (fh->f)) // is there a file to read from
                {
                    if (fh->format->detach_queue_block)
                        fh->format->detach_queue_block (NULL, client->refbuf);
                    refbuf_release (client->refbuf);
                    client->refbuf = NULL;
                    client->pos = 0;
                    client->intro_offset = fh->frame_start_pos;
                    if (fh->finfo.limit)
                    {
                        client->ops = &throttled_file_content_ops;
                        rate_add (fh->out_bitrate, 0, worker->time_ms);
                        return 0;
                    }
                    client->ops = &file_content_ops;
                    return client->ops->process (client);
                }
                if (client->respcode)
                    return -1;
                return client_send_404 (client, NULL);
            }
            else
            {
                refbuf_t *to_go = client->refbuf;
                refbuf = client->refbuf = to_go->next;
                to_go->next = NULL;
                if (fh->format && fh->format->detach_queue_block)
                    fh->format->detach_queue_block (NULL, client->refbuf);
                refbuf_release (to_go);
            }
            client->pos = 0;
        }
        if (refbuf->flags & BUFFER_CONTAINS_HDR)
            bytes = format_generic_write_to_client (client);
        else
            bytes = client->check_buffer (client);
        if (bytes > 0)
        {
            written += bytes;
            global_add_bitrates (global.out_bitrate, bytes, worker->time_ms);
        }
        if (bytes < 0)
        {
            client->schedule_ms = worker->time_ms + (written ? 150 : 300);
            break;
        }
        if (written > 30000)
            break;
    }
    return 0;
}


/* fast send routine */
static int file_send (client_t *client)
{
    int loop = 6, bytes, written = 0;
    fh_node *fh = client->shared_data;
    worker_t *worker = client->worker;
    time_t now;

#if 0
    if (fserve_change_worker (client)) // allow for balancing
        return 1;
#endif
    client->schedule_ms = worker->time_ms;
    now = worker->current_time.tv_sec;
    /* slowdown if max bandwidth is exceeded, but allow for short-lived connections to avoid
     * this, eg admin requests */
    if (throttle_sends > 1 && now - client->connection.con_time > 1)
    {
        client->schedule_ms += 300;
        loop = 1;
    }
    while (loop && written < 48000)
    {
        loop--;
        if (fserve_running == 0 || client->connection.error)
            return -1;
        if (format_file_read (client, fh->format, fh->f) < 0)
            return -1;
        bytes = client->check_buffer (client);
        if (bytes < 0)
        {
            client->schedule_ms += (written ? 80 : 150);
            return 0;
        }
        written += bytes;
    }
    client->schedule_ms += 4;
    return 0;
}



/* send routine for files sent at a target bitrate, eg fallback files. */
static int throttled_file_send (client_t *client)
{
    int  bytes;
    fh_node *fh = client->shared_data;
    time_t now;
    worker_t *worker = client->worker;
    unsigned long secs;
    unsigned int  rate = 0;
    unsigned int limit = fh->finfo.limit;

    if (fserve_running == 0 || client->connection.error)
        return -1;
    now = worker->current_time.tv_sec;
    secs = now - client->timer_start;
    client->schedule_ms = worker->time_ms;
    if (fh->finfo.override)
        return fserve_move_listener (client);

    if (fserve_change_worker (client)) // allow for balancing
        return 1;

    if (client->flags & CLIENT_WANTS_FLV) /* increase limit for flv clients as wrapping takes more space */
        limit = (unsigned long)(limit * 1.01);
    rate = secs ? (client->counter+1400)/secs : limit * 2;
    // DEBUG3 ("counter %lld, duration %ld, limit %u", client->counter, secs, rate);
    if (rate > limit)
    {
        if (limit >= 1400)
            client->schedule_ms += 1000/(limit/1400);
        else
            client->schedule_ms += 50; // should not happen but guard against it
        rate_add (fh->out_bitrate, 0, worker->time_ms);
        global_add_bitrates (global.out_bitrate, 0, worker->time_ms);
        if (client->counter > 8192)
            return 0; // allow an initial amount without throttling
    }
    switch (format_file_read (client, fh->format, fh->f))
    {
        case -1: // DEBUG0 ("loop of file triggered");
            client->intro_offset = fh->frame_start_pos;
            client->schedule_ms += client->throttle ? client->throttle : 150;
            return 0;
        case -2: // DEBUG0 ("major failure on read, better leave");
            return -1;
        default: //DEBUG1 ("reading from offset %ld", client->intro_offset);
            break;
    }
    bytes = client->check_buffer (client);
    if (bytes < 0)
        bytes = 0;
    //DEBUG3 ("bytes %d, counter %ld, %ld", bytes, client->counter, client->worker->time_ms - (client->timer_start*1000));
    rate_add (fh->out_bitrate, bytes, worker->time_ms);
    global_add_bitrates (global.out_bitrate, bytes, worker->time_ms);
    if (limit > 2800)
        client->schedule_ms += (1000/(limit/1400*2));
    else
        client->schedule_ms += 50;

    /* progessive slowdown if max bandwidth is exceeded. */
    if (throttle_sends > 1)
        client->schedule_ms += 300;
    return 0;
}


struct _client_functions throttled_file_content_ops =
{
    throttled_file_send,
    file_release
};


int fserve_setup_client_fb (client_t *client, fbinfo *finfo)
{
    fh_node *fh = &no_file;
    int ret = 0;

    if (finfo)
    {
        mount_proxy *minfo;
        if ((finfo->flags & FS_MISSING) || ((finfo->flags & FS_FALLBACK) && finfo->limit == 0))
            return -1;

        minfo = config_lock_mount (config_get_config(), finfo->mount);
        avl_tree_wlock (fh_cache);
        fh = find_fh (finfo);

        if (fh)
        {
            config_release_config();
            thread_mutex_lock (&fh->lock);
            avl_tree_unlock (fh_cache);
            client->shared_data = NULL;
            if (minfo)
            {
                if (minfo->max_listeners >= 0 && fh->refcount > minfo->max_listeners)
                {
                    thread_mutex_unlock (&fh->lock);
                    config_release_mount (minfo);
                    return client_send_403redirect (client, finfo->mount, "max listeners reached");
                }
                if (check_duplicate_logins (finfo->mount, fh->clients, client, minfo->auth) == 0)
                {
                    thread_mutex_unlock (&fh->lock);
                    config_release_mount (minfo);
                    return client_send_403 (client, "Account already in use");
                }
            }
            config_release_mount (minfo);
        }
        else
        {
            if (minfo && minfo->max_listeners == 0)
            {
                config_release_config();
                config_release_mount (minfo);
                avl_tree_unlock (fh_cache);
                client->shared_data = NULL;
                return client_send_403redirect (client, finfo->mount, "max listeners reached");
            }
            fh = open_fh (finfo, minfo);
            if (fh == NULL)
            {
                config_release_mount (minfo);
                finfo->flags |= FS_MISSING;
                return client_send_404 (client, NULL);
            }
            config_release_mount (minfo);
            if (fh->finfo.limit)
                DEBUG2 ("request for throttled file %s (bitrate %" PRIu64 ")", fh->finfo.mount, fh->finfo.limit*8);
        }
        if (fh->finfo.limit)
        {
            client->timer_start = time (NULL);
            if (client->connection.sent_bytes == 0)
                client->timer_start -= 2;
            client->counter = 0;
            global_reduce_bitrate_sampling (global.out_bitrate);
        }
    }
    else
    {
        if (client->mount && (client->flags & CLIENT_AUTHENTICATED) && (client->respcode >= 300 || client->respcode < 200))
        {
            fh = calloc (1, sizeof (no_file));
            fh->finfo.mount = strdup (client->mount);
            fh->finfo.flags |= FS_DELETE;
            fh->refcount = 1;
            fh->f = SOCK_ERROR;
            thread_mutex_create (&fh->lock);
        }
        thread_mutex_lock (&fh->lock);
    }
    client->mount = fh->finfo.mount;
    do
    {
        if (client->respcode) break;
        ret = -1;

        off_t f_range = (fh->finfo.fsize - fh->frame_start_pos);
        char fsize [20];

        snprintf (fsize, 20, "%" PRId64, (int64_t)f_range);
        httpp_setvar (client->parser, "__FILESIZE", fsize);

        if (client->connection.flags & CONN_FLG_END_UNSPEC)
            client->connection.discon.sent = f_range;
        else if (client->connection.discon.sent > f_range)
            break;
        client->connection.discon.sent -= client->connection.start_pos;

        ice_http_t http = ICE_HTTP_INIT;
        http.in_length = client->connection.discon.sent;
        if (fh->finfo.limit)
            client->flags &= ~CLIENT_KEEPALIVE; // file loops so drop keep alive
        if (fh->finfo.type == FORMAT_TYPE_UNDEFINED)
        {
            ret = format_client_headers (fh->format, &http,  client);
        }
        else
        {
            if (fh->format->create_client_data && client->format_data == NULL)
                ret = fh->format->create_client_data (fh->format, &http, client);
            if (fh->format->write_buf_to_client)
                client->check_buffer = fh->format->write_buf_to_client;
            ret = 0;
        }
        ice_http_complete (&http);
    } while (0);
    if (ret < 0)
    {
        thread_mutex_unlock (&fh->lock);
        client->mount = NULL;
        return client_send_416 (client);
    }
    fh_add_client (fh, client);
    client->shared_data = fh;
    thread_mutex_unlock (&fh->lock);

    if (client->check_buffer == NULL)
        client->check_buffer = format_generic_write_to_client;

    client->ops = &buffer_content_ops;
    client->flags |= CLIENT_IN_FSERVE;

    client->flags &= ~CLIENT_HAS_INTRO_CONTENT;
    client_add_incoming (client);

    return 0;
}


int client_http_send (ice_http_t *http)
{
    client_t *client = http->client;
    ice_http_complete (http);
    return fserve_setup_client (client);
}


int fserve_setup_client (client_t *client)
{
    client->check_buffer = format_generic_write_to_client;
    return fserve_setup_client_fb (client, NULL);
}


int fserve_set_override (const char *mount, const char *dest, format_type_t type)
{
    fh_node fh, *result;

    fh.finfo.flags = FS_FALLBACK;
    fh.finfo.mount = (char *)mount;
    fh.finfo.override = NULL;
    fh.finfo.type = type;

    avl_tree_wlock (fh_cache);
    result = find_fh (&fh.finfo);
    if (result)
    {
        thread_mutex_lock (&result->lock);

        if (result->refcount > 0)
        {       // insert clean copy, no stats or listeners
            fh_node *copy = calloc (1, sizeof (*copy));
            avl_delete (fh_cache, result, NULL);
            copy->finfo = result->finfo;
            copy->finfo.mount = strdup (copy->finfo.mount);
            copy->prev_count = -1; // trigger stats update
            copy->expire = (time_t)-1;
            copy->stats = 0;
            copy->format = result->format;
            copy->f = result->f;
            thread_mutex_create (&copy->lock);
            copy->out_bitrate = rate_setup (10000, 1000);
            copy->clients = avl_tree_new (client_compare, NULL);
            avl_insert (fh_cache, copy);

            // leave detached from cache, last listener trigger delete
            result->finfo.flags |= FS_DELETE;
            result->finfo.flags &= ~FS_FALLBACK;
            result->format = NULL;
            result->f = SOCK_ERROR;
            result->finfo.override = strdup (dest);
            result->finfo.type = type;
        }
        fh_stats (result, 0);
        avl_tree_unlock (fh_cache);
        thread_mutex_unlock (&result->lock);
        INFO2 ("move clients from %s to %s", mount, dest);
        return 1;
    }
    avl_tree_unlock (fh_cache);
    return 0;
}

static int _delete_mapping(void *mapping) {
    mime_type *map = mapping;
    free(map->ext);
    free(map->type);
    free(map);

    return 1;
}

static int _compare_mappings(void *arg, void *a, void *b)
{
    return strcmp(
            ((mime_type *)a)->ext,
            ((mime_type *)b)->ext);
}


// write filename extension for matching mime type.
// lookup matching mime type and write extension into buffer space provided
void fserve_write_mime_ext (const char *mimetype, char *buf, unsigned int len)
{
    avl_node *node;
    int semi;

    if (mimetype == NULL || buf == NULL || len > 2000) return;
    semi = strcspn (mimetype, "; ");
    if (semi == 0) return;
    if (mimetype [semi])
    {
        char *mt = alloca (++semi);
        snprintf (mt, semi, "%s", mimetype);
        mimetype = (const char *)mt;
    }
    thread_mutex_lock (&pending_lock);
    node = avl_get_first (mimetypes);
    while (node)
    {
       mime_type *mime = (mime_type *)node->key;
       if (mime && strcmp (mime->type, mimetype) == 0)
       {
           snprintf (buf, len, "%s", mime->ext);
           break;
       }
       node = avl_get_next (node);
    }
    thread_mutex_unlock (&pending_lock);
}


void fserve_recheck_mime_types (ice_config_t *config)
{
    mime_type *mapping;
    int i;
    avl_tree *old_mimetypes = NULL, *new_mimetypes = avl_tree_new(_compare_mappings, NULL);

    mime_type defaults[] = {
        { "m3u",            "audio/x-mpegurl" },
        { "pls",            "audio/x-scpls" },
        { "xspf",           "application/xspf+xml" },
        { "ogg",            "application/ogg" },
        { "xml",            "text/xml" },
        { "mp3",            "audio/mpeg" },
        { "aac",            "audio/aac" },
        { "aacp",           "audio/aacp" },
        { "css",            "text/css" },
        { "txt",            "text/plain" },
        { "html",           "text/html" },
        { "jpg",            "image/jpg" },
        { "png",            "image/png" },
        { "gif",            "image/gif" },
        { NULL, NULL }
    };

    for (i=0; defaults[i].ext; i++)
    {
        mapping = malloc (sizeof(mime_type));
        mapping->ext = strdup (defaults [i].ext);
        mapping->type = strdup (defaults [i].type);
        if (avl_insert (new_mimetypes, mapping) != 0)
            _delete_mapping (mapping);
    }
    do
    {
        char *type, *ext, *cur;
        FILE *mimefile = NULL;
        char line[4096];

        if (config->mimetypes_fn == NULL)
        {
            INFO0 ("no mime types file defined, using defaults");
            break;
        }
        mimefile = fopen (config->mimetypes_fn, "r");
        if (mimefile == NULL)
        {
            WARN1 ("Cannot open mime types file %s, using defaults", config->mimetypes_fn);
            break;
        }
        while (fgets(line, sizeof line, mimefile))
        {
            line[4095] = 0;

            if(*line == 0 || *line == '#')
                continue;

            type = line;
            cur = line;

            while(*cur != ' ' && *cur != '\t' && *cur)
                cur++;
            if(*cur == 0)
                continue;

            *cur++ = 0;

            while(1)
            {
                while(*cur == ' ' || *cur == '\t')
                    cur++;
                if(*cur == 0)
                    break;

                ext = cur;
                while(*cur != ' ' && *cur != '\t' && *cur != '\n' && *cur)
                    cur++;
                *cur++ = 0;
                if(*ext)
                {
                    /* Add a new extension->type mapping */
                    mapping = malloc(sizeof(mime_type));
                    mapping->ext = strdup(ext);
                    mapping->type = strdup(type);
                    if (avl_insert (new_mimetypes, mapping) != 0)
                        _delete_mapping (mapping);
                }
            }
        }
        fclose(mimefile);
    } while (0);

    thread_mutex_lock (&pending_lock);
    old_mimetypes = mimetypes;
    mimetypes = new_mimetypes;
    thread_mutex_unlock (&pending_lock);
    if (old_mimetypes)
        avl_tree_free (old_mimetypes, _delete_mapping);
}


int fserve_kill_client (client_t *client, const char *mount, int response)
{
    int loop = 2;
    uint64_t id;
    fbinfo finfo;
    xmlDocPtr doc;
    xmlNodePtr node;
    const char *idtext, *v = "0";
    char buf[50];

    finfo.flags = 0;
    finfo.mount = (char*)mount;
    finfo.limit = 0;
    finfo.override = NULL;

    idtext = httpp_get_query_param (client->parser, "id");
    if (idtext == NULL)
        return client_send_400 (client, "missing parameter id");

    if (sscanf (idtext, "%" SCNuMAX, &id) != 1)
        return client_send_400 (client, "unable to handle id");

    doc = xmlNewDoc(XMLSTR("1.0"));
    node = xmlNewDocNode(doc, NULL, XMLSTR("iceresponse"), NULL);
    xmlDocSetRootElement(doc, node);
    snprintf (buf, sizeof(buf), "Client %" PRIuMAX " not found", id);

    avl_tree_rlock (fh_cache);
    while (1)
    {
        avl_node *node;
        fh_node *fh = find_fh (&finfo);
        if (fh)
        {
            thread_mutex_lock (&fh->lock);
            avl_tree_unlock (fh_cache);
            node = avl_get_first (fh->clients);
            while (node)
            {
                client_t *listener = (client_t *)node->key;
                if (listener->connection.id == id)
                {
                    listener->connection.error = 1;
                    snprintf (buf, sizeof(buf), "Client %" PRIu64 " removed", id);
                    v = "1";
                    loop = 0;
                    break;
                }
                node = avl_get_next (node);
            }
            thread_mutex_unlock (&fh->lock);
            avl_tree_rlock (fh_cache);
        }
        if (loop == 0) break;
        loop--;
        if (loop == 1) finfo.flags = FS_FALLBACK;
    }
    avl_tree_unlock (fh_cache);
    xmlNewChild (node, NULL, XMLSTR("message"), XMLSTR(buf));
    xmlNewChild (node, NULL, XMLSTR("return"), XMLSTR(v));
    return admin_send_response (doc, client, response, "response.xsl");
}


int fserve_list_clients_xml (xmlNodePtr parent, fbinfo *finfo)
{
    int ret = 0;
    fh_node *fh;
    avl_node *anode;

    avl_tree_rlock (fh_cache);
    fh = find_fh (finfo);
    if (fh == NULL)
    {
        avl_tree_unlock (fh_cache);
        return 0;
    }
    thread_mutex_lock (&fh->lock);
    avl_tree_unlock (fh_cache);

    anode = avl_get_first (fh->clients);
    while (anode)
    {
        client_t *listener = (client_t *)anode->key;

        stats_listener_to_xml (listener, parent);
        ret++;
        anode = avl_get_next (anode);
    }
    thread_mutex_unlock (&fh->lock);
    return ret;
}


int fserve_list_clients (client_t *client, const char *mount, int response, int show_listeners)
{
    int ret;
    fbinfo finfo;
    xmlDocPtr doc;
    xmlNodePtr node, srcnode;

    finfo.flags = 0;
    finfo.mount = (char*)mount;
    finfo.limit = 0;
    finfo.override = NULL;

    doc = xmlNewDoc(XMLSTR("1.0"));
    node = xmlNewDocNode(doc, NULL, XMLSTR("icestats"), NULL);
    xmlDocSetRootElement(doc, node);
    srcnode = xmlNewChild(node, NULL, XMLSTR("source"), NULL);
    xmlSetProp(srcnode, XMLSTR("mount"), XMLSTR(mount));

    ret = fserve_list_clients_xml (srcnode, &finfo);
    if (ret == 0 && finfo.flags&FS_FALLBACK)
    {
        finfo.flags = 0; // retry
        ret = fserve_list_clients_xml (srcnode, &finfo);
    }
    if (ret)
    {
        char buf [20];
        snprintf (buf, sizeof(buf), "%d", ret);
        xmlNewChild (srcnode, NULL, XMLSTR("listeners"), XMLSTR(buf));
        return admin_send_response (doc, client, response, "listclients.xsl");
    }
    xmlFreeDoc (doc);
    return client_send_400 (client, "mount does not exist");
}


int fserve_query_count (fbinfo *finfo, mount_proxy *mountinfo)
{
    int ret = -1;
    fh_node *fh;

    if (finfo->flags & FS_FALLBACK && finfo->limit)
    {
        config_get_config();
        avl_tree_wlock (fh_cache);

        fh = open_fh (finfo, mountinfo);
        if (fh)
        {
            ret = fh->refcount;
            if (ret == 0)
                fh->expire = time(NULL)+20;
            thread_mutex_unlock (&fh->lock);
        }
    }
    else
    {
        avl_tree_rlock (fh_cache);

        fh = find_fh (finfo);
        if (fh)
        {
            thread_mutex_lock (&fh->lock);
            ret = fh->refcount;
            thread_mutex_unlock (&fh->lock);
        }
        avl_tree_unlock (fh_cache);
    }
    return ret;
}


int file_in_use (icefile_handle f)
{
    return f != -1;
}


void file_close (icefile_handle *f)
{
   if (*f != -1)
       close (*f);
   *f = -1;
}


int file_open (icefile_handle *f, const char *fn)
{
    *f = open (fn, O_RDONLY|O_CLOEXEC|O_BINARY);
    return (*f) < 0 ? -1 : 0;
}


#ifndef HAVE_PREAD
ssize_t pread (icefile_handle f, void *data, size_t count, off_t offset)
{
    ssize_t bytes = -1;

    // we do not want another thread to modifiy handle between seek and read
    // win32 may be able to use the overlapped io struct in ReadFile
    thread_mutex_lock (&seekread_lock);
    if (lseek (f, offset, SEEK_SET) != (off_t)-1)
        bytes = read (f, data, count);
    thread_mutex_unlock (&seekread_lock);
    return bytes;
}
#endif


void fserve_scan (time_t now)
{
    avl_node *node;

    global_lock();
    if (global.running != ICE_RUNNING)
        now = (time_t)0;
    global_unlock();

    avl_tree_wlock (fh_cache);
    node = avl_get_first (fh_cache);
    while (node)
    {
        fh_node *fh = node->key;
        node = avl_get_next (node);

        thread_mutex_lock (&fh->lock);

        if (now == (time_t)0)
        {
            fh->expire = 0;
            thread_mutex_unlock (&fh->lock);
            continue;
        }

        if (fh->finfo.limit)
        {
            if (fh->stats)
            {
                stats_lock (fh->stats, NULL);
                if (fh->prev_count != fh->refcount)
                {
                    fh->prev_count = fh->refcount;
                    stats_set_args (fh->stats, "listeners", "%ld", fh->refcount);
                    stats_set_args (fh->stats, "listener_peak", "%ld", fh->peak);
                }
                if (fh->stats_update <= now)
                {
                    fh->stats_update = now + 5;
                    stats_set_args (fh->stats, "outgoing_kbitrate", "%ld",
                            (long)((8 * rate_avg (fh->out_bitrate))/1024));
                }
                stats_release (fh->stats);
            }
        }

        if (fh->refcount == 0 && fh->expire >= 0 && now >= fh->expire)
        {
            DEBUG1 ("timeout of %s", fh->finfo.mount);
            fh_stats (fh, 0);
            remove_fh_from_cache (fh);
            thread_mutex_unlock (&fh->lock);
            _delete_fh (fh);
            continue;
        }
        thread_mutex_unlock (&fh->lock);
    }
    avl_tree_unlock (fh_cache);
}



// return 0 for missing, 1 for found, -1 try again as we would block
//
int fserve_contains (const char *name)
{
    int ret = 0;
    fbinfo finfo;

    memset (&finfo, 0, sizeof (finfo));
    if (strncmp (name, "fallback-/", 10) == 0)
    {
        finfo.mount = (char*)name+9;
        finfo.flags = FS_FALLBACK;
    }
    else if (strncmp (name, "file-/", 6) == 0)
        finfo.mount = (char*)name;
    if (avl_tree_tryrlock (fh_cache) < 0)
        return -1;
    DEBUG1 ("looking for %s", name);
    if (find_fh (&finfo))
       ret = 1;
    avl_tree_unlock (fh_cache);
    return ret;
}

