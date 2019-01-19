/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2019 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"

#include "zend_arena.h"
#include "zend_exceptions.h"
#include "zend_vm.h"
#include "zend_vm_opcodes.h"

#include "php_pcov.h"

#define PCOV_FILTER_ALL     0
#define PCOV_FILTER_INCLUDE 1
#define PCOV_FILTER_EXCLUDE 2

#define PHP_PCOV_UNCOVERED -1
#define PHP_PCOV_COVERED    1

#ifndef GC_ADDREF
#	define GC_ADDREF(g) ++GC_REFCOUNT(g)
#endif

static zval php_pcov_uncovered;
static zval php_pcov_covered;

void (*zend_execute_ex_function)(zend_execute_data *execute_data);

ZEND_DECLARE_MODULE_GLOBALS(pcov)

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN(
		"pcov.enabled", "1", 
		PHP_INI_SYSTEM, OnUpdateBool, 
		ini.enabled, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY  (
		"pcov.directory", "/", 
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateString, 
		ini.directory, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY(
		"pcov.initial.memory", "65336", 
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateLong, 
		ini.memory, zend_pcov_globals, pcov_globals)
	STD_PHP_INI_ENTRY(
		"pcov.initial.files", "64", 
		PHP_INI_SYSTEM | PHP_INI_PERDIR, OnUpdateLong, 
		ini.files, zend_pcov_globals, pcov_globals)
PHP_INI_END()

static PHP_GINIT_FUNCTION(pcov)
{
#if defined(COMPILE_DL_PCOV) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	ZEND_SECURE_ZERO(pcov_globals, sizeof(zend_pcov_globals));
}

static zend_always_inline zend_bool php_pcov_wants(zend_string *filename) {
	if (!PCG(directory)) {
		return 1;
	}

	if (ZSTR_LEN(filename) < ZSTR_LEN(PCG(directory))) {
		return 0;
	}

	if (zend_hash_exists(&PCG(wants), filename)) {
		return 1;
	}

	if (zend_hash_exists(&PCG(ignores), filename)) {
		return 0;
	}

	if (strncmp(
		ZSTR_VAL(filename), 
		ZSTR_VAL(PCG(directory)), 
		ZSTR_LEN(PCG(directory))) == SUCCESS) {
		zend_hash_add_empty_element(&PCG(wants), filename);
		return 1;
	}

	zend_hash_add_empty_element(&PCG(ignores), filename);
	return 0;
}

static zend_always_inline zend_bool php_pcov_ignored_opcode(const zend_op *opline, zend_uchar opcode) { /* {{{ */
	if (opline->lineno < 1) {
		return 1;
	}
	
	return
	    opcode == ZEND_NOP || 
	    opcode == ZEND_OP_DATA || 
 	    opcode == ZEND_FE_FREE || 
	    opcode == ZEND_FREE || 
	    opcode == ZEND_ASSERT_CHECK ||
	    opcode == ZEND_VERIFY_RETURN_TYPE ||  
	    opcode == ZEND_RECV ||
	    opcode == ZEND_RECV_INIT ||
	    opcode == ZEND_RECV_VARIADIC ||
	    opcode == ZEND_SEND_VAL ||
	    opcode == ZEND_SEND_VAR_EX ||
	    opcode == ZEND_SEND_REF ||
	    opcode == ZEND_SEND_UNPACK ||
	    opcode == ZEND_DECLARE_CONST || 
	    opcode == ZEND_DECLARE_CLASS || 
	    opcode == ZEND_DECLARE_INHERITED_CLASS || 
	    opcode == ZEND_DECLARE_FUNCTION || 
	    opcode == ZEND_DECLARE_INHERITED_CLASS_DELAYED || 
	    opcode == ZEND_DECLARE_ANON_CLASS || 
	    opcode == ZEND_DECLARE_ANON_INHERITED_CLASS || 
	    opcode == ZEND_FAST_RET || 
	    opcode == ZEND_FAST_CALL ||
	    opcode == ZEND_TICKS || 
	    opcode == ZEND_EXT_STMT || 
	    opcode == ZEND_EXT_FCALL_BEGIN || 
	    opcode == ZEND_EXT_FCALL_END || 
	    opcode == ZEND_EXT_NOP || 
#if PHP_VERSION_ID < 70400
	    opcode == ZEND_VERIFY_ABSTRACT_CLASS || 
	    opcode == ZEND_ADD_TRAIT || 
	    opcode == ZEND_BIND_TRAITS || 
#endif
	    opcode == ZEND_BIND_GLOBAL
	;
} /* }}} */

static zend_always_inline php_coverage_t* php_pcov_create(zend_execute_data *execute_data) { /* {{{ */
	php_coverage_t *coverage = (php_coverage_t*) zend_arena_alloc(&PCG(mem), sizeof(php_coverage_t));

	coverage->file     = zend_string_copy(EX(func)->op_array.filename);
	coverage->line     = EX(opline)->lineno;
	coverage->next     = NULL;

	return (PCG(create) = coverage);
} /* }}} */

static zend_always_inline zend_bool php_pcov_needs(zend_execute_data *execute_data) { /* {{{ */
	if (php_pcov_ignored_opcode(EX(opline), EX(opline)->opcode)) {
		return 0;
	}

	if (PCG(create) &&
	    PCG(create)->file == EX(func)->op_array.filename &&
	    PCG(create)->line == EX(opline)->lineno) {
		return 0;
	}

	return php_pcov_wants(EX(func)->op_array.filename);
} /* }}} */

static zend_always_inline int php_pcov_trace(zend_execute_data *execute_data) { /* {{{ */
	if (PCG(enabled) && php_pcov_needs(execute_data)) {
		php_coverage_t *coverage = php_pcov_create(execute_data);

		if (!PCG(start)) {
			PCG(start) = coverage;
		} else {
			*(PCG(next)) = coverage;
		}

		PCG(next) = &coverage->next;
	}

	return zend_vm_call_opcode_handler(execute_data);
} /* }}} */

static zend_always_inline void php_pcov_cache(zend_op_array *result) { /* {{{ */
	zend_op_array *mapped;

	if (!result->filename || !php_pcov_wants(result->filename)) {
		return;
	}

	if (zend_hash_exists(&PCG(files), result->filename)) {
		return;
	}

	mapped = (zend_op_array*) zend_hash_add_mem(
				&PCG(files), 
				result->filename, 
				result, sizeof(zend_op_array));

	if (mapped->static_variables) {
		if (!(GC_FLAGS(mapped->static_variables) & IS_ARRAY_IMMUTABLE)) {
			GC_ADDREF(mapped->static_variables);
		}
	}

	mapped->refcount = NULL;
#if PHP_VERSION_ID >= 70400
	ZEND_MAP_PTR_INIT(mapped->run_time_cache, NULL);
#else
	mapped->run_time_cache = NULL;
#endif
} /* }}} */

void php_pcov_execute_ex(zend_execute_data *execute_data) { /* {{{ */
	int zrc        = 0;

	if (!EX(func)->common.function_name) {
		php_pcov_cache((zend_op_array*) EX(func));
	}

	while (1) {
		if (EX(opline)->opcode == ZEND_DO_FCALL || 
		    EX(opline)->opcode == ZEND_DO_UCALL ||
		    EX(opline)->opcode == ZEND_DO_FCALL_BY_NAME ||
		    EX(opline)->opcode == ZEND_USER_FUNCTION) {
			zend_execute_ex = execute_ex;
		}

		zrc = php_pcov_trace(execute_data);

		if (zend_execute_ex != php_pcov_execute_ex) {
			zend_execute_ex = php_pcov_execute_ex;
		}

		if (zrc != SUCCESS) {
			if (zrc < SUCCESS) {
				return;
			}
			execute_data = EG(current_execute_data);
		}
	}
} /* }}} */

void php_pcov_files_dtor(zval *zv) { /* {{{ */
	destroy_op_array(Z_PTR_P(zv));
	efree(Z_PTR_P(zv));
} /* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(pcov)
{
	REGISTER_NS_LONG_CONSTANT("pcov", "all",         PCOV_FILTER_ALL,    CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("pcov", "inclusive",   PCOV_FILTER_INCLUDE, CONST_CS|CONST_PERSISTENT);
	REGISTER_NS_LONG_CONSTANT("pcov", "exclusive",   PCOV_FILTER_EXCLUDE, CONST_CS|CONST_PERSISTENT);

	REGISTER_INI_ENTRIES();

	if (INI_BOOL("pcov.enabled")) {
		zend_execute_ex_function   = zend_execute_ex;
		zend_execute_ex            = php_pcov_execute_ex;
	}

	ZVAL_LONG(&php_pcov_uncovered, PHP_PCOV_UNCOVERED);
	ZVAL_LONG(&php_pcov_covered,   PHP_PCOV_COVERED);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(pcov)
{
	if (INI_BOOL("pcov.enabled")) {
		zend_execute_ex   = zend_execute_ex_function;
	}

	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(pcov)
{
#if defined(COMPILE_DL_PCOV) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif

	if (!INI_BOOL("pcov.enabled")) {
		return SUCCESS;
	}

	PCG(mem) = zend_arena_create(INI_INT("pcov.initial.memory"));

	zend_hash_init(&PCG(files),      INI_INT("pcov.initial.files"), NULL, php_pcov_files_dtor, 0);
	zend_hash_init(&PCG(ignores),    INI_INT("pcov.initial.files"), NULL, NULL, 0);
	zend_hash_init(&PCG(wants),      INI_INT("pcov.initial.files"), NULL, NULL, 0);
	zend_hash_init(&PCG(discovered), INI_INT("pcov.initial.files"), NULL, ZVAL_PTR_DTOR, 0);

	if (INI_STR("pcov.directory")) {
		PCG(directory) = zend_string_init(
			INI_STR("pcov.directory"), 
			strlen(INI_STR("pcov.directory")), 0);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(pcov)
{
	if (!INI_BOOL("pcov.enabled") || CG(unclean_shutdown)) {
		return SUCCESS;
	}

	if (PCG(start)) {
		php_coverage_t *coverage = PCG(start);
		do {
			zend_string_release(coverage->file);
		} while (coverage = coverage->next);
	}

	zend_hash_destroy(&PCG(files));
	zend_hash_destroy(&PCG(ignores));
	zend_hash_destroy(&PCG(wants));
	zend_hash_destroy(&PCG(discovered));

	zend_arena_destroy(PCG(mem));

	if (PCG(directory)) {
		zend_string_release(PCG(directory));
	}

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(pcov)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pcov support", "enabled");
	php_info_print_table_end();
}
/* }}} */

static zend_always_inline void php_pcov_report(php_coverage_t *coverage, zval *filter) { /* {{{ */
	if (!coverage) {
		return;
	}

	do {
		zval *table = zend_hash_find(Z_ARRVAL_P(filter), coverage->file);

		if (table) {
			zval *hit = zend_hash_index_find(Z_ARRVAL_P(table), coverage->line);

			if (hit) {
				Z_LVAL_P(hit) = PHP_PCOV_COVERED;
			} else {
				/* 
					note: during discovery we ignore opcodes that are not ignored
					      during tracing, SEND/RECV and the implicit return
				*/
				zend_hash_index_add(Z_ARRVAL_P(table), coverage->line, &php_pcov_covered);
			}
		}
	} while (coverage = coverage->next);
} /* }}} */

static zend_always_inline void php_pcov_discover_code(zend_op_array *ops, zval *return_value) { /* {{{ */
	zend_op       *opline = ops->opcodes, 
		      *end    = ops->opcodes + ops->last;

	if ((ops->last) >= 1 && ((end - 1)->opcode == ZEND_RETURN ||
			         (end - 1)->opcode == ZEND_RETURN_BY_REF ||
			         (end - 1)->opcode == ZEND_GENERATOR_RETURN) &&
	    (ops->last > 1) &&  ((end - 2)->opcode == ZEND_RETURN ||
			         (end - 2)->opcode == ZEND_RETURN_BY_REF ||
			         (end - 2)->opcode == ZEND_GENERATOR_RETURN ||
			         (end - 2)->opcode == ZEND_VERIFY_RETURN_TYPE)) {
		if (ops->function_name == NULL || (end - 1)->extended_value == -1) {
			end -= (end - 2)->opcode == ZEND_VERIFY_RETURN_TYPE ?
					2 : 1;
		}
	}

	while (opline < end) {
		if (php_pcov_ignored_opcode(opline, opline->opcode)) {
			opline++;
			continue;
		}

		if (!zend_hash_index_exists(Z_ARRVAL_P(return_value), opline->lineno)) {
			zend_hash_index_add(
				Z_ARRVAL_P(return_value), 
				opline->lineno, &php_pcov_uncovered);
		}

		if ((opline +0)->opcode == ZEND_NEW && 
		    (opline +1)->opcode == ZEND_DO_FCALL) {
			opline++;
		}

		opline++;
	}
} /* }}} */

static zend_always_inline void php_pcov_discover_file(zend_string *file, zval *return_value) { /* {{{ */
	zval discovered;
	zend_op_array *ops;
	zval *cache = zend_hash_find(&PCG(discovered), file);

	if (cache) {
		zend_hash_update(
			Z_ARRVAL_P(return_value), file, cache);
		Z_ADDREF_P(cache);
		return;
	}

	if (!(ops = zend_hash_find_ptr(&PCG(files), file))) {
		return;
	}

	array_init(&discovered);

	php_pcov_discover_code(ops, &discovered);
	{
		zend_class_entry *ce;
		zend_op_array    *function;
		ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
			if (ce->type != ZEND_USER_CLASS) {
				continue;
			}

			ZEND_HASH_FOREACH_PTR(&ce->function_table, function) {
				if (function->type == ZEND_USER_FUNCTION &&
				    function->filename &&
				    zend_string_equals(file, function->filename)) {
					php_pcov_discover_code(function, &discovered);
				}
			} ZEND_HASH_FOREACH_END();
		} ZEND_HASH_FOREACH_END();
	}

	{
		zend_op_array *function;
		ZEND_HASH_FOREACH_PTR(EG(function_table), function) {
			if (function->type == ZEND_USER_FUNCTION &&
			    function->filename &&
			    zend_string_equals(file, function->filename)) {
				php_pcov_discover_code(function, &discovered);
			}
		} ZEND_HASH_FOREACH_END();
	}

	zend_hash_update(Z_ARRVAL_P(return_value), file, &discovered);
	zend_hash_update(&PCG(discovered), file, &discovered);
	Z_ADDREF(discovered);
} /* }}} */

/* {{{ array \pcov\collect(int $type = \pcov\all, array $filter = []); */
PHP_NAMED_FUNCTION(php_pcov_collect)
{
	zend_long type = PCOV_FILTER_ALL;
	zval      *filter = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|la", &type, &filter) != SUCCESS) {
		return;
	}

	if (PCOV_FILTER_ALL != type &&
	    PCOV_FILTER_INCLUDE != type &&
	    PCOV_FILTER_EXCLUDE != type) {
		zend_throw_error(zend_ce_type_error, 
			"type must be "
				"\\pcov\\inclusive, "
				"\\pcov\\exclusive, or \\pcov\\all");
		return;
	}

	array_init(return_value);

	if (PCG(last) == PCG(next)) {
		return;
	}

	PCG(last) = PCG(next);

	switch(type) {
		case PCOV_FILTER_INCLUDE: {
			zval *filtered;
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
				if (Z_TYPE_P(filtered) != IS_STRING) {
					continue;
				}

				php_pcov_discover_file(Z_STR_P(filtered), return_value);
			} ZEND_HASH_FOREACH_END();
		} break;

		case PCOV_FILTER_EXCLUDE: {
			zend_string *name;
			zval *filtered;
			ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(filter), filtered) {
					if (Z_TYPE_P(filtered) != IS_STRING) {
						continue;
					}

					if (zend_string_equals(name, Z_STR_P(filtered))) {
						goto _php_pcov_collect_exclude;
					}
				} ZEND_HASH_FOREACH_END();				
				php_pcov_discover_file(name, return_value);

			_php_pcov_collect_exclude:
				continue;
			} ZEND_HASH_FOREACH_END();
		} break;

		case PCOV_FILTER_ALL: {
			zend_string *name;
			ZEND_HASH_FOREACH_STR_KEY(&PCG(files), name) {
				php_pcov_discover_file(name, return_value);
			} ZEND_HASH_FOREACH_END();
		} break;
	}

	php_pcov_report(PCG(start), return_value);
} /* }}} */

/* {{{ void \pcov\start(void) */
PHP_NAMED_FUNCTION(php_pcov_start)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	if (!INI_BOOL("pcov.enabled")) {
		return;
	}

	PCG(enabled) = 1;
} /* }}} */

/* {{{ void \pcov\stop(void) */
PHP_NAMED_FUNCTION(php_pcov_stop)
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;
	}

	if (!INI_BOOL("pcov.enabled")) {
		return;
	}

	PCG(enabled) = 0;
} /* }}} */

/* {{{ void \pcov\clear(bool $files = false) */
PHP_NAMED_FUNCTION(php_pcov_clear)
{
	zend_bool files = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &files) != SUCCESS) {
		return;
	}

	if (!INI_BOOL("pcov.enabled")) {
		return;
	}

	if (PCG(start)) {
		php_coverage_t *coverage = PCG(start);
		do {
			zend_string_release(coverage->file);
		} while (coverage = coverage->next);
	}

	if (files) {
		PCG(includes) = NULL;

		zend_hash_clean(&PCG(files));
		zend_hash_clean(&PCG(discovered));
	}

	zend_arena_destroy(PCG(mem));

	PCG(mem) = zend_arena_create(INI_INT("pcov.initial.memory"));
	PCG(start) = NULL;
	PCG(create) = NULL;
} /* }}} */

/* {{{ array \pcov\includes(void) */
PHP_NAMED_FUNCTION(php_pcov_includes) 
{
	if (zend_parse_parameters_none() != SUCCESS) {
		return;	
	}

	array_init(return_value);

	if (!PCG(includes)) {
		PCG(includes) = EG(included_files).arData;
	}

	{
		Bucket *bucket = PCG(includes);
		Bucket *end = EG(included_files).arData + 
			      EG(included_files).nNumUsed;

		while (bucket < end) {
			add_next_index_str(
				return_value, 
				zend_string_copy(bucket->key));
			bucket++;
		}

		PCG(includes) = end;
	}
} /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_collect_arginfo, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, filter, IS_ARRAY, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_clear_arginfo, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, files, _IS_BOOL, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_pcov_no_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO() /* }}} */

/* {{{ php_pcov_functions[]
 */
const zend_function_entry php_pcov_functions[] = {
	ZEND_NS_FENTRY("pcov", start,      php_pcov_start,         php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", stop,       php_pcov_stop,          php_pcov_no_arginfo, 0)
	ZEND_NS_FENTRY("pcov", collect,    php_pcov_collect,       php_pcov_collect_arginfo, 0)
	ZEND_NS_FENTRY("pcov", clear,      php_pcov_clear,         php_pcov_clear_arginfo, 0)
	ZEND_NS_FENTRY("pcov", includes,   php_pcov_includes,      php_pcov_no_arginfo, 0)
	PHP_FE_END
};
/* }}} */

/* {{{ pcov_module_entry
 */
zend_module_entry pcov_module_entry = {
	STANDARD_MODULE_HEADER,
	"pcov",
	php_pcov_functions,
	PHP_MINIT(pcov),
	PHP_MSHUTDOWN(pcov),
	PHP_RINIT(pcov),
	PHP_RSHUTDOWN(pcov),
	PHP_MINFO(pcov),
	PHP_PCOV_VERSION,
	PHP_MODULE_GLOBALS(pcov),
	PHP_GINIT(pcov),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_PCOV
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(pcov)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
