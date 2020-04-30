/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2010 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrei Zmievski <andrei@php.net>                            |
  +----------------------------------------------------------------------+
*/

/* TODO
 * parse client Id in constructor
 * add version to MINFO
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if HAVE_PTHREAD
#include <pthread.h>
#endif

#include <php.h>
#include <php_ticks.h>

#ifdef ZTS
#include "TSRM.h"
#endif

#include <php_ini.h>
#include <SAPI.h>
#include <ext/standard/info.h>
#include <zend_extensions.h>

#if PHP_MAJOR_VERSION < 7
#include <ext/standard/php_smart_str.h>
#else
#include <ext/standard/php_smart_string.h>
#endif

#include "php5to7.h"
#include "php_zookeeper.h"
#include "php_zookeeper_private.h"
#include "php_zookeeper_session.h"
#include "php_zookeeper_exceptions.h"
#include "php_zookeeper_class.h"
#include "php_zookeeper_config_class.h"
#include "php_zookeeper_stat.h"
#include "php_zookeeper_callback.h"
#include "php_zookeeper_log.h"

/****************************************
  Helper macros
****************************************/

#define ZK_METHOD_INIT_VARS                \
    zval*             object  = getThis(); \
    php_zk_t*         i_obj   = NULL;      \

#define ZK_METHOD_FETCH_OBJECT                                                 \
    i_obj = Z_ZK_P(object);   \
	if (!i_obj->zk) {	\
		php_zk_throw_exception(PHPZK_CONNECT_NOT_CALLED TSRMLS_CC); \
		return;	\
	} \

/****************************************
  Structures and definitions
****************************************/

static zend_class_entry *zookeeper_ce = NULL;

static zend_object_handlers zookeeper_obj_handlers;

static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_ZOOKEEPER_SESSION
static int le_zookeeper_connection;
#endif

#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION < 3)
const zend_fcall_info empty_fcall_info = { 0, NULL, NULL, NULL, NULL, 0, NULL, NULL, 0 };
#undef ZEND_BEGIN_ARG_INFO_EX
#define ZEND_BEGIN_ARG_INFO_EX(name, pass_rest_by_reference, return_reference, required_num_args)   \
    static zend_arg_info name[] = {                                                                       \
        { NULL, 0, NULL, 0, 0, 0, pass_rest_by_reference, return_reference, required_num_args },
#endif

ZEND_DECLARE_MODULE_GLOBALS(zookeeper)

#ifdef COMPILE_DL_ZOOKEEPER
ZEND_GET_MODULE(zookeeper)
#endif

/****************************************
  Forward declarations
****************************************/
static void php_zk_node_watcher_marshal(zhandle_t *zk, int type, int state, const char *path, void *context);
static void php_zk_completion_marshal(int rc, const void *context);
static void php_parse_acl_list(zval *z_acl, struct ACL_vector *aclv);
static void php_aclv_destroy(struct ACL_vector *aclv);
static void php_aclv_to_array(const struct ACL_vector *aclv, zval *array);
static void php_zk_dispatch();
static void php_zk_close(php_zk_t *i_obj TSRMLS_DC);


/****************************************
  Async
****************************************/

#if PHP_MAJOR_VERSION >= 7 && PHP_MINOR_VERSION >= 1
static void (*orig_interrupt_function)(zend_execute_data *execute_data);
static void php_zk_interrupt_function(zend_execute_data *execute_data)
{
	php_zk_dispatch();
	if (orig_interrupt_function) {
		orig_interrupt_function(execute_data);
	}
}
#endif

/****************************************
  Helper functions
****************************************/

static inline php_zk_t* php_zk_fetch_object(zend_object *obj) {
#ifdef ZEND_ENGINE_3
	return (php_zk_t *)((char*)(obj) - XtOffsetOf(php_zk_t, zo));
#else
	return (php_zk_t *) obj;
#endif
}

/****************************************
  Method implementations
****************************************/

static void php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAMETERS, char *host, zend_fcall_info *fci, zend_fcall_info_cache *fcc, long recv_timeout)
{
	zval *object = getThis();
	php_zk_t *i_obj;
	zhandle_t *zk = NULL;
	php_cb_data_t *cb_data = NULL;

	if (recv_timeout <= 0) {
		php_zk_throw_exception(ZBADARGUMENTS TSRMLS_CC);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "recv_timeout parameter has to be greater than 0");
#ifndef ZEND_ENGINE_3
		ZVAL_NULL(object);
#endif
		return;
	}

	i_obj = (php_zk_t *) Z_ZK_P(object);

	if (fci->size != 0) {
		cb_data = php_cb_data_new(&i_obj->callbacks, fci, fcc, 0 TSRMLS_CC);
	}
	zk = zookeeper_init(host, (fci->size != 0) ? php_zk_watcher_marshal : NULL,
						recv_timeout, 0, cb_data, 0);

	if (zk == NULL) {
		php_zk_throw_exception(PHPZK_INITIALIZATION_FAILURE TSRMLS_CC);
		/* not reached */
#ifndef ZEND_ENGINE_3
		ZVAL_NULL(object);
#endif
		return;
	}

	i_obj->zk = zk;
	i_obj->cb_data = cb_data;
}

/* {{{ Zookeeper::connect ( .. )
   Connects to a zookeeper host */
static PHP_METHOD(Zookeeper, connect)
{
	strsize_t host_len;
	char *host;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	long recv_timeout = ZK_G(recv_timeout);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!l", &host, &host_len, &fci, &fcc, &recv_timeout) == FAILURE) {
		return;
	}

	php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, host, &fci, &fcc, recv_timeout);
}
/* }}} */


/* {{{ Zookeeper::__construct ( .. )
   Creates a Zookeeper object and optionally connects */
static PHP_METHOD(Zookeeper, __construct)
{
	zval *object = getThis();
	strsize_t host_len = 0;
	char *host = NULL;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	long recv_timeout = ZK_G(recv_timeout);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sf!l", &host, &host_len, &fci, &fcc, &recv_timeout) == FAILURE) {
#ifndef ZEND_ENGINE_3
		ZVAL_NULL(object);
#endif
		return;
	}

	if (host_len > 0)
	{
		php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, host, &fci, &fcc, recv_timeout);
	}

}
/* }}} */


/* {{{ Zookeeper::create( .. )
   */
static PHP_METHOD(Zookeeper, create)
{
	char *path, *value = NULL;
	strsize_t path_len, value_len;
	zval *acl_info = NULL;
	long flags = 0;
	char *realpath;
	int realpath_max = 0;
	struct ACL_vector aclv = { 0, };
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss!a!|l", &path, &path_len,
							  &value, &value_len, &acl_info, &flags) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	realpath_max = path_len + 1;
	if (flags & ZOO_SEQUENCE) {
		// allocate extra space for sequence numbers
		realpath_max += 11;
	}
	realpath = emalloc(realpath_max);

	if (value == NULL) {
		value_len = -1;
	}

	php_parse_acl_list(acl_info, &aclv);
	status = zoo_create(i_obj->zk, path, value, value_len, (acl_info ? &aclv : 0), flags,
						realpath, realpath_max);
	php_aclv_destroy(&aclv);
	if (status != ZOK) {
		efree(realpath);
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	PHP5TO7_RETVAL_STRING(realpath);
	efree(realpath);
}
/* }}} */

/* {{{ Zookeeper::delete( .. )
   */
static PHP_METHOD(Zookeeper, delete)
{
	char *path;
	strsize_t path_len;
	long version = -1;
	int status = ZOK;

	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &path, &path_len,
							  &version) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	status = zoo_delete(i_obj->zk, path, version);
	if (status != ZOK) {
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getChildren( .. )
   */
static PHP_METHOD(Zookeeper, getChildren)
{
	char *path;
	strsize_t path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	struct String_vector strings;
	int i, status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!", &path, &path_len, &fci,
							  &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&i_obj->callbacks, &fci, &fcc, 1 TSRMLS_CC);
	}
	status = zoo_wget_children(i_obj->zk, path,
							   (fci.size != 0) ? php_zk_node_watcher_marshal : NULL,
							   cb_data, &strings);
	if (status != ZOK) {
		php_cb_data_destroy(&cb_data);
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	array_init(return_value);
	for (i = 0; i < strings.count; i++) {
		php5to7_add_next_index_string(return_value, strings.data[i]);
	}
    deallocate_String_vector(&strings);
}
/* }}} */

/* {{{ Zookeeper::get( .. )
   */
static PHP_METHOD(Zookeeper, get)
{
	char *path;
	strsize_t path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	zval *stat_info = NULL;
	php_cb_data_t *cb_data = NULL;
	char *buffer;
	long max_size = 0;
	struct Stat stat;
	int status = ZOK;
	int length;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!zl", &path, &path_len, &fci,
							  &fcc, &stat_info, &max_size) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

#ifdef ZEND_ENGINE_3
	if (stat_info) {
		ZVAL_DEREF(stat_info);
	}
#endif

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&i_obj->callbacks, &fci, &fcc, 1 TSRMLS_CC);
	}

	if (max_size <= 0) {
		status = zoo_exists(i_obj->zk, path, 0, &stat);/* I think we don't need zk->watcher any more */

		if (status != ZOK) {
			php_cb_data_destroy(&cb_data);
			php_zk_throw_exception(status TSRMLS_CC);
			return;
		}
		length = stat.dataLength;
	} else {
		length = max_size;
	}

	if (length <= 0) {/* znode carries a NULL */
		if (stat_info) {
			php_stat_to_array(&stat, stat_info);
		}

		php_cb_data_destroy(&cb_data);
		RETURN_NULL();
	}

	php_zk_log_info(i_obj->zk, "path=%s, cb_data=%p", path, cb_data);

	buffer = emalloc (length+1);
	status = zoo_wget(i_obj->zk, path, (fci.size != 0) ? php_zk_node_watcher_marshal : NULL,
					  cb_data, buffer, &length, &stat);
	buffer[length] = 0;

	if (status != ZOK) {
		efree (buffer);
		php_cb_data_destroy(&cb_data);
		php_zk_throw_exception(status TSRMLS_CC);

		/* Indicate data marshalling failure with boolean false so that user can retry */
		if (status == ZMARSHALLINGERROR) {
			RETURN_FALSE;
		}
		return;
	}

	if (stat_info) {
		php_stat_to_array(&stat, stat_info);
	}

	/* Length will be returned as -1 if the znode carries a NULL */
	if (length == -1) {
		php_cb_data_destroy(&cb_data);
		RETURN_NULL();
	}

	PHP5TO7_RETVAL_STRINGL(buffer, length);
	efree(buffer);
}
/* }}} */

/* {{{ Zookeeper::exists( .. )
   */
static PHP_METHOD(Zookeeper, exists)
{
	char *path;
	strsize_t path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	struct Stat stat;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!", &path, &path_len, &fci,
							  &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&i_obj->callbacks, &fci, &fcc, 1 TSRMLS_CC);
	}
	status = zoo_wexists(i_obj->zk, path, (fci.size != 0) ? php_zk_node_watcher_marshal : NULL,
						 cb_data, &stat);
	if (status != ZOK && status != ZNONODE) {
		php_cb_data_destroy(&cb_data);
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	if (status == ZOK) {
		php_stat_to_array(&stat, return_value);
		return;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ Zookeeper::set( .. )
   */
static PHP_METHOD(Zookeeper, set)
{
	char *path, *value = NULL;
	strsize_t path_len, value_len;
	long version = -1;
	zval *stat_info = NULL;
	struct Stat stat, *stat_ptr = NULL;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss!|lz", &path, &path_len,
							  &value, &value_len, &version, &stat_info) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (stat_info) {
#ifdef ZEND_ENGINE_3
		ZVAL_DEREF(stat_info);
#endif
		stat_ptr = &stat;
	}
	if (value == NULL) {
		value_len = -1;
	}
	status = zoo_set2(i_obj->zk, path, value, value_len, version, stat_ptr);
	if (status != ZOK) {
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	if (stat_info) {
		php_stat_to_array(stat_ptr, stat_info);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getClientId( .. )
   */
static PHP_METHOD(Zookeeper, getClientId)
{
	const clientid_t *cid;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	cid = zoo_client_id(i_obj->zk);
	array_init(return_value);
	add_next_index_long(return_value, cid->client_id);
	php5to7_add_next_index_string(return_value, (char *)cid->passwd);
}
/* }}} */

/* {{{ Zookeeper::getAcl( .. )
   */
static PHP_METHOD(Zookeeper, getAcl)
{
	char *path;
	strsize_t path_len;
	int status = ZOK;
	struct ACL_vector aclv;
	struct Stat stat;
	zval *stat_info, *acl_info;
#ifdef ZEND_ENGINE_3
	zval _stat_info = {0}, _acl_info = {0};
#endif
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &path, &path_len) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	status = zoo_get_acl(i_obj->zk, path, &aclv, &stat);
	if (status != ZOK) {
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

#ifdef ZEND_ENGINE_3
	stat_info = &_stat_info;
	acl_info = &_acl_info;
#else
	MAKE_STD_ZVAL(acl_info);
	MAKE_STD_ZVAL(stat_info);
#endif
	php_aclv_to_array(&aclv, acl_info);
	php_stat_to_array(&stat, stat_info);
	array_init(return_value);
	add_next_index_zval(return_value, stat_info);
	add_next_index_zval(return_value, acl_info);
}
/* }}} */

/* {{{ Zookeeper::setAcl( .. )
   */
static PHP_METHOD(Zookeeper, setAcl)
{
	char *path;
	strsize_t path_len;
	long version;
	zval *acl_info;
	struct ACL_vector aclv;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sla", &path, &path_len,
							  &version, &acl_info) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	php_parse_acl_list(acl_info, &aclv);
	status = zoo_set_acl(i_obj->zk, path, version, &aclv);
	php_aclv_destroy(&aclv);
	if (status != ZOK) {
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getState( .. )
   */
static PHP_METHOD(Zookeeper, getState)
{
	int state;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	state = zoo_state(i_obj->zk);
	RETURN_LONG(state);
}

/* {{{ Zookeeper::getRecvTimeout( .. )
   */
static PHP_METHOD(Zookeeper, getRecvTimeout)
{
	int recv_timeout;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	recv_timeout = zoo_recv_timeout(i_obj->zk);
	RETURN_LONG(recv_timeout);
}

/* {{{ Zookeeper::isRecoverable( .. )
   */
static PHP_METHOD(Zookeeper, isRecoverable)
{
	int result;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	result = is_unrecoverable(i_obj->zk);
	RETURN_BOOL(!result);
}

/* {{{ Zookeeper::setDebugLevel( .. )
   */
static PHP_METHOD(Zookeeper, setDebugLevel)
{
	long level;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &level) == FAILURE) {
		return;
	}

	zoo_set_debug_level((ZooLogLevel)level);
	RETURN_TRUE;
}

/* {{{ Zookeeper::setDeterministicConnOrder( .. )
   */
static PHP_METHOD(Zookeeper, setDeterministicConnOrder)
{
	zend_bool value;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "b", &value) == FAILURE) {
		return;
	}

	zoo_deterministic_conn_order(value);
	RETURN_TRUE;
}

/* {{{ Zookeeper::addAuth( .. )
   */
static PHP_METHOD(Zookeeper, addAuth)
{
	char *scheme, *cert;
	strsize_t scheme_len, cert_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	int status = ZOK;
	php_cb_data_t *cb_data = NULL;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|f", &scheme, &scheme_len, &cert,
							  &cert_len, &fci, &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&i_obj->callbacks, &fci, &fcc, 0 TSRMLS_CC);
	}
	status = zoo_add_auth(i_obj->zk, scheme, cert, cert_len,
						  (fci.size != 0) ? php_zk_completion_marshal : NULL, cb_data);
	if (status != ZOK) {
		php_zk_throw_exception(status TSRMLS_CC);
		return;
	}

	RETURN_TRUE;
}

/* }}} */

/* {{{ Zookeeper::setWatcher( .. )
   */
static PHP_METHOD(Zookeeper, setWatcher)
{
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "f", &fci, &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (i_obj->cb_data) {
		zend_hash_index_del(&i_obj->callbacks, i_obj->cb_data->h);
	}
	cb_data = php_cb_data_new(&i_obj->callbacks, &fci, &fcc, 0 TSRMLS_CC);
	zoo_set_watcher(i_obj->zk, php_zk_watcher_marshal);
	i_obj->cb_data = cb_data;

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::setLogStream( .. )
   */
static PHP_METHOD(Zookeeper, setLogStream)
{
	zval *zstream;
	php_stream *stream;
	FILE *fp;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zstream) == FAILURE) {
		return;
	}

	if (Z_TYPE_P(zstream) == IS_RESOURCE) {
#ifdef ZEND_ENGINE_3
		php_stream_from_zval(stream, zstream);
#else
		php_stream_from_zval(stream, &zstream);
#endif
	} else {

#ifdef ZEND_ENGINE_3
		convert_to_string_ex(zstream);
#else
		convert_to_string_ex(&zstream);
#endif
		stream = php_stream_open_wrapper(Z_STRVAL_P(zstream), "w", REPORT_ERRORS, NULL);
	}
	if (stream == NULL) {
		RETURN_FALSE;
	}

	if (FAILURE == php_stream_cast(stream, PHP_STREAM_AS_STDIO, (void **) &fp, REPORT_ERRORS)) {
		RETURN_FALSE;
	}

	zoo_set_log_stream(fp);

	if (Z_TYPE_P(zstream) == IS_STRING) {
		php_stream_free(stream, PHP_STREAM_FREE_CLOSE_CASTED);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::close ( .. )
   Close the zookeeper handle and free up any resources */
static PHP_METHOD(Zookeeper, close)
{
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	php_zk_close(i_obj TSRMLS_CC);
}
/* }}} */

#if ZOO_MAJOR_VERSION>=3 && ZOO_MINOR_VERSION>=5
/* {{{ Zookeeper::getConfig( .. )
   */
static PHP_METHOD(Zookeeper, getConfig)
{
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	RETURN_OBJ(php_zk_config_new_from_zk(php_zk_config_ce, i_obj));
}
/* }}} */
#endif

PHP_FUNCTION(zookeeper_dispatch)
{
    php_zk_dispatch();
}

/****************************************
  Internal support code
****************************************/

/* {{{ constructor/destructor */
static void php_zk_close(php_zk_t *i_obj TSRMLS_DC)
{
	php_cb_data_t *cb_data_p;

	if (i_obj->cb_data) {
		php_cb_data_destroy(&i_obj->cb_data);
		i_obj->cb_data = NULL;
	}

	if (i_obj->zk) {
		zookeeper_close(i_obj->zk);
		i_obj->zk = NULL;
	}

	zend_hash_clean(&i_obj->callbacks);
}

static void php_zk_destroy(php_zk_t *i_obj TSRMLS_DC)
{
	php_zk_close(i_obj TSRMLS_CC);
	zend_hash_destroy(&i_obj->callbacks);
}

static void php_zk_free_storage(zend_object *obj TSRMLS_DC)
{
	php_zk_t *i_obj;

	i_obj = php_zk_fetch_object(obj);
	zend_object_std_dtor(&i_obj->zo TSRMLS_CC);
	php_zk_destroy(i_obj TSRMLS_CC);
}

#ifdef ZEND_ENGINE_3
static void php_cb_data_zv_destroy(zval *entry)
{
	if( Z_TYPE_P(entry) == IS_PTR ) {
		php_cb_data_destroy(Z_PTR_P(entry)); // php_cb_data_t
		efree(Z_PTR_P(entry)); // Allocated by zend_hash_next_index_insert_mem()
	}
}
#endif

#ifdef ZEND_ENGINE_3
zend_object* php_zk_new(zend_class_entry *ce TSRMLS_DC)
{
	php_zk_t *i_obj;

	i_obj = ecalloc(1, sizeof(php_zk_t) + zend_object_properties_size(ce));
	zend_object_std_init( &i_obj->zo, ce TSRMLS_CC );
	i_obj->zo.handlers = &zookeeper_obj_handlers;

	zend_hash_init_ex(&i_obj->callbacks, 5, NULL, (dtor_func_t)php_cb_data_zv_destroy, 0, 0);

	return &i_obj->zo;
}
#else
zend_object_value php_zk_new(zend_class_entry *ce TSRMLS_DC)
{
	zend_object_value retval;
	php_zk_t *i_obj;
	zval *tmp;

	i_obj = ecalloc(1, sizeof(*i_obj));
	zend_object_std_init( &i_obj->zo, ce TSRMLS_CC );
#if PHP_VERSION_ID < 50399
	zend_hash_copy(i_obj->zo.properties, &ce->default_properties, (copy_ctor_func_t) zval_add_ref, (void *) &tmp, sizeof(zval *));
#else
	object_properties_init( (zend_object *) i_obj, ce);
#endif

	retval.handle = zend_objects_store_put(i_obj, (zend_objects_store_dtor_t)zend_objects_destroy_object, (zend_objects_free_object_storage_t)php_zk_free_storage, NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();

	zend_hash_init_ex(&i_obj->callbacks, 5, NULL, (dtor_func_t)php_cb_data_destroy, 0, 0);

	return retval;
}
#endif

static inline void php_zk_dispatch_one(php_cb_data_t *cb_data, int type, int state, const char *path TSRMLS_DC)
{
#ifdef ZEND_ENGINE_3
	zval params[3];
	zval retval = {0};

	ZVAL_LONG(&params[0], type);
	ZVAL_LONG(&params[1], state);
	PHP5TO7_ZVAL_STRING(&params[2], (char *)path);

	cb_data->fci.retval = &retval;
#else
	zval **params[3];
	zval *retval;
	zval *z_type;
	zval *z_state;
	zval *z_path;

	MAKE_STD_ZVAL(z_type);
	MAKE_STD_ZVAL(z_state);
	MAKE_STD_ZVAL(z_path);

	ZVAL_LONG(z_type, type);
	ZVAL_LONG(z_state, state);
	PHP5TO7_ZVAL_STRING(z_path, (char *)path);

	params[0] = &z_type;
	params[1] = &z_state;
	params[2] = &z_path;

	cb_data->fci.retval_ptr_ptr = &retval;
#endif

	cb_data->fci.params = params;
	cb_data->fci.param_count = 3;

	if (zend_call_function(&cb_data->fci, &cb_data->fcc TSRMLS_CC) == SUCCESS) {
		zval_ptr_dtor(&retval);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not invoke watcher callback");
	}

#ifdef ZEND_ENGINE_3
	zval_ptr_dtor(&params[2]);
#else
	zval_ptr_dtor(&z_type);
	zval_ptr_dtor(&z_state);
	zval_ptr_dtor(&z_path);
#endif

	if (cb_data->oneshot) {
		zend_hash_index_del(cb_data->ht, cb_data->h);
	}
}

static inline void php_zk_dispatch_one_completion(php_cb_data_t *cb_data, int rc TSRMLS_DC)
{
#ifdef ZEND_ENGINE_3
	zval params[3];
	zval retval = {0};

	ZVAL_LONG(&params[0], rc);

	cb_data->fci.retval = &retval;
#else
	zval **params[1];
	zval *retval;
	zval *z_rc;

	MAKE_STD_ZVAL(z_rc);

	ZVAL_LONG(z_rc, rc);

	params[0] = &z_rc;

	cb_data->fci.retval_ptr_ptr = &retval;
#endif

	cb_data->fci.params = params;
	cb_data->fci.param_count = 1;

	if (zend_call_function(&cb_data->fci, &cb_data->fcc TSRMLS_CC) == SUCCESS) {
		zval_ptr_dtor(&retval);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not invoke completion callback");
	}

#ifdef ZEND_ENGINE_3
#else
	zval_ptr_dtor(&z_rc);
#endif

	if (cb_data->oneshot) {
		zend_hash_index_del(cb_data->ht, cb_data->h);
	}
}

static void php_zk_dispatch()
{
	TSRMLS_FETCH();

	struct php_zk_pending_marshal *queue;
	struct php_zk_pending_marshal *next;

	if( !ZK_G(pending_marshals) ) {
		return;
	}

	// Bail if the queue is empty or if we are already playing the queue
	if( !ZK_G(head) || ZK_G(processing_marshal_queue) ) {
		return;
	}

#if HAVE_PTHREAD
	pthread_mutex_lock(&cb_lock);
#endif

	// Prevent reentrant handler calls
	ZK_G(processing_marshal_queue) = 1;

	queue = ZK_G(head);
	ZK_G(head) = NULL; /* simple stores are atomic */

	while( queue ) {
		// Process
		if( queue->is_completion ) {
			php_zk_dispatch_one_completion(queue->cb_data, queue->rc TSRMLS_CC);
		} else {
			php_zk_dispatch_one(queue->cb_data, queue->type, queue->state, queue->path TSRMLS_CC);
		}

		// Move
		next = queue->next;
		free(queue->path);
		free(queue);
		queue = next;
	}

	ZK_G(processing_marshal_queue) = 0;
	ZK_G(pending_marshals) = 0;

#if HAVE_PTHREAD
	pthread_mutex_unlock(&cb_lock);
#endif

}

static void php_zk_node_watcher_marshal(zhandle_t *zk, int type, int state, const char *path, void *context)
{
	// When client loses its connection or has just reconnected to the server,
	// it also calls this function with type=ZOO_SESSION_EVENT and path=NULL for each watcher of node
	// So I think we should skip this watcher we really don't need.
	if (type == ZOO_SESSION_EVENT
			&& (!path
					|| !strlen(path)))
		return;

	php_zk_watcher_marshal(zk, type, state, path, context);
}

void php_zk_watcher_marshal(zhandle_t *zk, int type, int state, const char *path, void *context)
{
	php_zk_log_debug(zk, "type=%d, state=%d, path=%s, path(p)=%p, context=%p", type, state, path?path:"", path, context);

	php_cb_data_t *cb_data = context;

#if HAVE_PTHREAD
	pthread_mutex_lock(&cb_lock);
#endif

#if ZTS
	void *prev = tsrm_set_interpreter_context(cb_data->ctx);
	TSRMLS_FETCH_FROM_CTX(cb_data->ctx);
#endif

	// Allocate new item
	struct php_zk_pending_marshal *p = calloc(1, sizeof(struct php_zk_pending_marshal));
	p->cb_data = context;
	p->type = type;
	p->state = state;
	p->path = strdup(path);
	p->cb_data = cb_data;

	// Add to list
	if( ZK_G(head) && ZK_G(tail) ) {
		ZK_G(tail)->next = p;
	} else {
		ZK_G(head) = p;
	}

	ZK_G(tail) = p;
	ZK_G(pending_marshals) = 1;

#if PHP_MAJOR_VERSION >= 7 && PHP_MINOR_VERSION >= 1
	EG(vm_interrupt) = 1;
#endif

#if ZTS
	tsrm_set_interpreter_context(prev);
#endif

#if HAVE_PTHREAD
	pthread_mutex_unlock(&cb_lock);
#endif
}

static void php_zk_completion_marshal(int rc, const void *context)
{
	php_zk_log_debug(NULL, "rc=%d, context=%p", rc, context);

	php_cb_data_t *cb_data = (php_cb_data_t *)context;

#if HAVE_PTHREAD
	pthread_mutex_lock(&cb_lock);
#endif

#if ZTS
	void *prev = tsrm_set_interpreter_context(cb_data->ctx);
	TSRMLS_FETCH_FROM_CTX(cb_data->ctx);
#endif

	// Allocate new item
	struct php_zk_pending_marshal *p = calloc(1, sizeof(struct php_zk_pending_marshal));
	p->is_completion = 1;
	p->rc = rc;
	p->cb_data = cb_data;

	// Add to list
	if( ZK_G(head) && ZK_G(tail) ) {
		ZK_G(tail)->next = p;
	} else {
		ZK_G(head) = p;
	}

	ZK_G(tail) = p;
	ZK_G(pending_marshals) = 1;

#if PHP_MAJOR_VERSION >= 7 && PHP_MINOR_VERSION >= 1
	EG(vm_interrupt) = 1;
#endif

#if ZTS
	tsrm_set_interpreter_context(prev);
#endif

#if HAVE_PTHREAD
	pthread_mutex_unlock(&cb_lock);
#endif
}

static void php_parse_acl_list(zval *z_acl, struct ACL_vector *aclv)
{
	int size = 0;
	int i = 0;
#ifdef ZEND_ENGINE_3
	ulong index = 0;
	zend_string *key;
	zval *entry = NULL;
	zval *perms = NULL;
	zval *scheme = NULL;
	zval *id = NULL;
#else
	zval **entry;
	zval **perms, **scheme, **id;
#endif

	if (!z_acl || (size = zend_hash_num_elements(Z_ARRVAL_P(z_acl))) == 0) {
		return;
	}

	aclv->data = (struct ACL *)calloc(size, sizeof(struct ACL));

#ifdef ZEND_ENGINE_3
	ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(z_acl), index, key, entry) {
		if( Z_TYPE_P(entry) != IS_ARRAY ) {
			continue;
		}

		perms = zend_hash_str_find(Z_ARRVAL_P(entry), ZEND_STRL("perms"));
		scheme = zend_hash_str_find(Z_ARRVAL_P(entry), ZEND_STRL("scheme"));
		id = zend_hash_str_find(Z_ARRVAL_P(entry), ZEND_STRL("id"));
		if (perms == NULL || scheme == NULL || id == NULL) {
			continue;
		}

		convert_to_long_ex(perms);
		convert_to_string_ex(scheme);
		convert_to_string_ex(id);

		aclv->data[i].perms = (int32_t)Z_LVAL_P(perms);
		aclv->data[i].id.id = strdup(Z_STRVAL_P(id));
		aclv->data[i].id.scheme = strdup(Z_STRVAL_P(scheme));

		i++;
	} ZEND_HASH_FOREACH_END();

#else
	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(z_acl));
		 zend_hash_get_current_data(Z_ARRVAL_P(z_acl), (void**)&entry) == SUCCESS;
		 zend_hash_move_forward(Z_ARRVAL_P(z_acl))) {

		if (Z_TYPE_PP(entry) != IS_ARRAY) {
			continue;
		}

		perms = scheme = id = NULL;
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("perms"), (void**)&perms);
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("scheme"), (void**)&scheme);
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("id"), (void**)&id);
		if (perms == NULL || scheme == NULL || id == NULL) {
			continue;
		}

		convert_to_long_ex(perms);
		convert_to_string_ex(scheme);
		convert_to_string_ex(id);

		aclv->data[i].perms = (int32_t)Z_LVAL_PP(perms);
		aclv->data[i].id.id = strdup(Z_STRVAL_PP(id));
		aclv->data[i].id.scheme = strdup(Z_STRVAL_PP(scheme));

		i++;
	}
#endif

	aclv->count = i;
}

static void php_aclv_destroy(struct ACL_vector *aclv)
{
	int i;
	for( i = 0; i < aclv->count; ++i ) {
		free(aclv->data[i].id.id);
		free(aclv->data[i].id.scheme);
	}
	free(aclv->data);
}

static void php_aclv_to_array(const struct ACL_vector *aclv, zval *array)
{
	int i;

	array_init(array);
	for (i = 0; i < aclv->count; i++) {
#ifdef ZEND_ENGINE_3
		zval _entry = {0};
		zval *entry = &_entry;
#else
		zval *entry;
		MAKE_STD_ZVAL(entry);
#endif
		array_init(entry);
		add_assoc_long_ex(entry, PHP5TO7_STRS("perms"), aclv->data[i].perms);
		php5to7_add_assoc_string_ex(entry, PHP5TO7_STRS("scheme"), aclv->data[i].id.scheme);
		php5to7_add_assoc_string_ex(entry, PHP5TO7_STRS("id"), aclv->data[i].id.id);
		add_next_index_zval(array, entry);
	}
}
/* }}} */

/* {{{ internal API functions */

PHP_ZOOKEEPER_API
zend_class_entry *php_zk_get_ce(void)
{
	return zookeeper_ce;
}

/* }}} */

/* {{{ methods arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(0, recv_timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_connect , 0, 0, 1)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(0, recv_timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_create, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_ARRAY_INFO(0, acl, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, version)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_getChildren, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_get, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(1, stat_info)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_exists, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_set, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, version)
	ZEND_ARG_INFO(1, stat_info)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getClientId, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getAcl, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setAcl, 0)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, version)
	ZEND_ARG_INFO(0, acl)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getState, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getRecvTimeout, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_isRecoverable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setDebugLevel, 0)
	ZEND_ARG_INFO(0, level)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setDeterministicConnOrder, 0)
	ZEND_ARG_INFO(0, trueOrFalse)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_addAuth, 0, 0, 2)
	ZEND_ARG_INFO(0, scheme)
	ZEND_ARG_INFO(0, cert)
	ZEND_ARG_INFO(0, completion_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setWatcher, 0)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setLogStream, 0)
	ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_close, 0)
ZEND_END_ARG_INFO()

#if ZOO_MAJOR_VERSION>=3 && ZOO_MINOR_VERSION>=5
ZEND_BEGIN_ARG_INFO(arginfo_getConfig, 0)
ZEND_END_ARG_INFO()
#endif
/* }}} */

/* {{{ zookeeper_class_methods */
#define ZK_ME(name, args) PHP_ME(Zookeeper, name, args, ZEND_ACC_PUBLIC)
#define ZK_ME_STATIC(name, args) PHP_ME(Zookeeper, name, args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
static zend_function_entry zookeeper_class_methods[] = {
	ZK_ME(__construct,        arginfo___construct)
	ZK_ME(connect,            arginfo_connect )

	ZK_ME(create,             arginfo_create)
	ZK_ME(delete,             arginfo_delete)
	ZK_ME(get,                arginfo_get)
	ZK_ME(getChildren,        arginfo_getChildren)
	ZK_ME(exists,             arginfo_exists)
	ZK_ME(set,                arginfo_set)

	ZK_ME(getAcl,             arginfo_getAcl)
	ZK_ME(setAcl,             arginfo_setAcl)

	ZK_ME(getClientId,        arginfo_getClientId)
	ZK_ME(getState,           arginfo_getState)
	ZK_ME(getRecvTimeout,     arginfo_getRecvTimeout)
	ZK_ME(isRecoverable,      arginfo_isRecoverable)

	ZK_ME_STATIC(setDebugLevel,      arginfo_setDebugLevel)
	ZK_ME_STATIC(setDeterministicConnOrder, arginfo_setDeterministicConnOrder)

	ZK_ME(addAuth,            arginfo_addAuth)

	ZK_ME(setWatcher,         arginfo_setWatcher)
	ZK_ME(setLogStream,       arginfo_setLogStream)

	ZK_ME(close,              arginfo_close)

#if ZOO_MAJOR_VERSION>=3 && ZOO_MINOR_VERSION>=5
	ZK_ME(getConfig,          arginfo_getConfig)
#endif

	PHP_FE_END
};
#undef ZK_ME
#undef ZK_ME_STATIC
/* }}} */

/* {{{ zookeeper_function_entry */
static const zend_function_entry zookeeper_functions[] = {
	PHP_FE(zookeeper_dispatch, NULL)
	PHP_FE_END
};
/* }}} */

/* {{{ zookeeper_module_entry */
zend_module_entry zookeeper_module_entry = {
#if ZEND_MODULE_API_NO >= 20050922
    	STANDARD_MODULE_HEADER_EX,
	NULL,
	NULL,
#else
    STANDARD_MODULE_HEADER,
#endif
	"zookeeper",
	zookeeper_functions,
	PHP_MINIT(zookeeper),
	PHP_MSHUTDOWN(zookeeper),
	PHP_RINIT(zookeeper),
	PHP_RSHUTDOWN(zookeeper),
	PHP_MINFO(zookeeper),
	PHP_ZOOKEEPER_VERSION,
	PHP_MODULE_GLOBALS(zookeeper),
	PHP_GINIT(zookeeper),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

/* {{{ php_zk_register_constants */
static void php_zk_register_constants(INIT_FUNC_ARGS)
{
	#define ZK_CLASS_CONST_LONG(name) zend_declare_class_constant_long(php_zk_get_ce() , ZEND_STRS( #name ) - 1, ZOO_##name TSRMLS_CC)
	#define ZK_CLASS_CONST_LONG2(name) zend_declare_class_constant_long(php_zk_get_ce() , ZEND_STRS( #name ) - 1, Z##name TSRMLS_CC)

	ZK_CLASS_CONST_LONG(PERM_READ);
	ZK_CLASS_CONST_LONG(PERM_WRITE);
	ZK_CLASS_CONST_LONG(PERM_CREATE);
	ZK_CLASS_CONST_LONG(PERM_DELETE);
	ZK_CLASS_CONST_LONG(PERM_ALL);
	ZK_CLASS_CONST_LONG(PERM_ADMIN);

	ZK_CLASS_CONST_LONG(EPHEMERAL);
	ZK_CLASS_CONST_LONG(SEQUENCE);

	ZK_CLASS_CONST_LONG(EXPIRED_SESSION_STATE);
	ZK_CLASS_CONST_LONG(AUTH_FAILED_STATE);
	ZK_CLASS_CONST_LONG(CONNECTING_STATE);
	ZK_CLASS_CONST_LONG(ASSOCIATING_STATE);
	ZK_CLASS_CONST_LONG(CONNECTED_STATE);

	/*
	 * zookeeper does not expose the symbol for the NOTCONNECTED state in the headers, so
	 * we have to cheat
	 */
	zend_declare_class_constant_long(php_zk_get_ce(), ZEND_STRS("NOTCONNECTED_STATE")-1, 999 TSRMLS_CC);

	ZK_CLASS_CONST_LONG(CREATED_EVENT);
	ZK_CLASS_CONST_LONG(DELETED_EVENT);
	ZK_CLASS_CONST_LONG(CHANGED_EVENT);
	ZK_CLASS_CONST_LONG(CHILD_EVENT);
	ZK_CLASS_CONST_LONG(SESSION_EVENT);
	ZK_CLASS_CONST_LONG(NOTWATCHING_EVENT);

	ZK_CLASS_CONST_LONG(LOG_LEVEL_ERROR);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_WARN);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_INFO);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_DEBUG);

	ZK_CLASS_CONST_LONG2(SYSTEMERROR);
	ZK_CLASS_CONST_LONG2(RUNTIMEINCONSISTENCY);
	ZK_CLASS_CONST_LONG2(DATAINCONSISTENCY);
	ZK_CLASS_CONST_LONG2(CONNECTIONLOSS);
	ZK_CLASS_CONST_LONG2(MARSHALLINGERROR);
	ZK_CLASS_CONST_LONG2(UNIMPLEMENTED);
	ZK_CLASS_CONST_LONG2(OPERATIONTIMEOUT);
	ZK_CLASS_CONST_LONG2(BADARGUMENTS);
	ZK_CLASS_CONST_LONG2(INVALIDSTATE);
#if ZOO_MAJOR_VERSION>=3 && ZOO_MINOR_VERSION>=5
	ZK_CLASS_CONST_LONG2(NEWCONFIGNOQUORUM);
	ZK_CLASS_CONST_LONG2(RECONFIGINPROGRESS);
#endif

	ZK_CLASS_CONST_LONG2(OK);
	ZK_CLASS_CONST_LONG2(APIERROR);
	ZK_CLASS_CONST_LONG2(NONODE);
	ZK_CLASS_CONST_LONG2(NOAUTH);
	ZK_CLASS_CONST_LONG2(BADVERSION);
	ZK_CLASS_CONST_LONG2(NOCHILDRENFOREPHEMERALS);
	ZK_CLASS_CONST_LONG2(NODEEXISTS);
	ZK_CLASS_CONST_LONG2(NOTEMPTY);
	ZK_CLASS_CONST_LONG2(SESSIONEXPIRED);
	ZK_CLASS_CONST_LONG2(INVALIDCALLBACK);
	ZK_CLASS_CONST_LONG2(INVALIDACL);
	ZK_CLASS_CONST_LONG2(AUTHFAILED);
	ZK_CLASS_CONST_LONG2(CLOSING);
	ZK_CLASS_CONST_LONG2(NOTHING);
	ZK_CLASS_CONST_LONG2(SESSIONMOVED);

	#undef  ZK_CLASS_CONST_LONG
	#undef  ZK_CLASS_CONST_LONG2
}
/* }}} */

#ifdef HAVE_ZOOKEEPER_SESSION
ZEND_RSRC_DTOR_FUNC(php_zookeeper_connection_dtor)
{
#ifdef ZEND_ENGINE_3
#define resptr res->ptr
#else
#define resptr rsrc->ptr
#endif

	if (resptr) {
		php_zookeeper_session *zk_sess = (php_zookeeper_session *)resptr;
		zookeeper_close(zk_sess->zk);
		pefree(zk_sess, 1);
		resptr = NULL;
	}

#undef resptr
}

int php_zookeeper_get_connection_le()
{
	return le_zookeeper_connection;
}
#endif

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("zookeeper.recv_timeout",		"10000",	PHP_INI_ALL,	OnUpdateLongGEZero,	recv_timeout,	zend_zookeeper_globals, zookeeper_globals)
#ifdef HAVE_ZOOKEEPER_SESSION
	STD_PHP_INI_ENTRY("zookeeper.session_lock",		"1",		PHP_INI_SYSTEM, OnUpdateBool,		session_lock,	zend_zookeeper_globals, zookeeper_globals)
	STD_PHP_INI_ENTRY("zookeeper.sess_lock_wait",	"150000",	PHP_INI_ALL,	OnUpdateLongGEZero,	sess_lock_wait,	zend_zookeeper_globals,	zookeeper_globals)
#endif
PHP_INI_END()

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(zookeeper)
{
	zend_class_entry ce;
#ifdef HAVE_ZOOKEEPER_SESSION
	le_zookeeper_connection = zend_register_list_destructors_ex(NULL, php_zookeeper_connection_dtor, "Zookeeper persistent connection (sessions)", module_number);
#endif
	INIT_CLASS_ENTRY(ce, "Zookeeper", zookeeper_class_methods);
	zookeeper_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zookeeper_ce->create_object = php_zk_new;

#ifdef ZEND_ENGINE_3
	memcpy(&zookeeper_obj_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	zookeeper_obj_handlers.offset = XtOffsetOf(php_zk_t, zo);
	zookeeper_obj_handlers.free_obj = php_zk_free_storage;
#endif

	/* set debug level to warning by default */
	zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);

	php_zk_register_constants(INIT_FUNC_ARGS_PASSTHRU);

	REGISTER_INI_ENTRIES();
#ifdef HAVE_ZOOKEEPER_SESSION
	php_session_register_module(ps_zookeeper_ptr);
#endif

	php_zk_register_exceptions(TSRMLS_C);

#if ZOO_MAJOR_VERSION>=3 && ZOO_MINOR_VERSION>=5
#ifdef ZEND_ENGINE_3
	php_zk_config_register(TSRMLS_C);
#endif
#endif

#if PHP_MAJOR_VERSION >= 7 && PHP_MINOR_VERSION >= 1
	orig_interrupt_function = zend_interrupt_function;
	zend_interrupt_function = php_zk_interrupt_function;
#elif defined(ZEND_ENGINE_3)
	php_add_tick_function(php_zk_dispatch, NULL);
#else
	php_add_tick_function(php_zk_dispatch);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(zookeeper)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(zookeeper)
{
	ZK_G(head) = ZK_G(tail) = NULL;

#if HAVE_PTHREAD
	pthread_mutex_init(&cb_lock, NULL);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(zookeeper)
{
	struct php_zk_pending_marshal *sig;

	while( ZK_G(head) ) {
		sig = ZK_G(head);
		ZK_G(head) = sig->next;
		free(sig);
	}

#if HAVE_PTHREAD
	pthread_mutex_destroy(&cb_lock);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_GINIT_FUNCTION */
PHP_GINIT_FUNCTION(zookeeper)
{
	memset(zookeeper_globals, 0, sizeof(*zookeeper_globals));
	zookeeper_globals->recv_timeout = 10000;
	zookeeper_globals->session_lock = 1;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(zookeeper)
{
	char buf[32];

	php_info_print_table_start();

	php_info_print_table_header(2, "zookeeper support", "enabled");
	php_info_print_table_row(2, "version", PHP_ZOOKEEPER_VERSION);

	snprintf(buf, sizeof(buf), "%ld.%ld.%ld", ZOO_MAJOR_VERSION, ZOO_MINOR_VERSION, ZOO_PATCH_VERSION);
	php_info_print_table_row(2, "libzookeeper version", buf);

	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim: noet sw=4 ts=4 fdm=marker:
 */
