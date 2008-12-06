#include "dbd_db2.h"

static lua_push_type_t db2_to_lua_push(unsigned int db2_type, int len) {
    lua_push_type_t lua_type;

    if (len == SQL_NULL_DATA)
	return LUA_PUSH_NIL;

    switch(db2_type) {
    case SQL_SMALLINT:
    case SQL_INTEGER:
	lua_type = LUA_PUSH_INTEGER; 
	break;
    case SQL_DECIMAL:
	lua_type = LUA_PUSH_NUMBER;
	break;
    default:
        lua_type = LUA_PUSH_STRING;
    }

    return lua_type;
}

/*
 * success = statement:close()
 */
static int statement_close(lua_State *L) {
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_DB2_STATEMENT);
    SQLRETURN rc = SQL_SUCCESS;

    if (statement->stmt) {
	rc = SQLFreeHandle(SQL_HANDLE_STMT, statement->stmt);

	if (statement->resultset) 
	    free(statement->resultset);

	if (statement->bind) {
	    int i;

	    for (i = 0; i < statement->num_result_columns; i++) {
		free(statement->bind[i].buffer);
	    }

	    free(statement->bind);
	}

	statement->num_result_columns = 0;
    }

    return 0;    
}

/*
 * success = statement:execute(...)
 */
static int statement_execute(lua_State *L) {
    int n = lua_gettop(L);
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_DB2_STATEMENT);
    int p;
    int i;
    int errflag = 0;
    const char *errstr = NULL;
    SQLRETURN rc = SQL_SUCCESS;
    unsigned char *buffer = NULL;
    int offset = 0;
    resultset_t *resultset = NULL; 
    bindparams_t *bind; /* variable to read the results */

    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH + 1];
    SQLCHAR sqlstate[SQL_SQLSTATE_SIZE + 1];
    SQLINTEGER sqlcode;
    SQLSMALLINT length;	

    if (!statement->stmt) {
	lua_pushboolean(L, 0);
	lua_pushstring(L, DBI_ERR_EXECUTE_INVALID);
	return 2;
    }

    for (p = 2; p <= n; p++) {
	int i = p - 1;
	int type = lua_type(L, p);
	char err[64];
	const char *str = NULL;
	size_t len = 0;
	double *num;
	int *boolean;
	const static SQLLEN nullvalue = SQL_NULL_DATA;

	switch(type) {
	case LUA_TNIL:
	    rc = SQLBindParameter(statement->stmt, i, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, (SQLPOINTER)0, 0, (SQLPOINTER)&nullvalue);
	    errflag = rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO;
	    break;
	case LUA_TNUMBER:
	    buffer = realloc(buffer, offset + sizeof(double));
	    num = (double *)buffer + offset;
	    *num = lua_tonumber(L, p);
	    offset += sizeof(double);
	    rc = SQLBindParameter(statement->stmt, i, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DECIMAL, 10, 0, (SQLPOINTER)num, 0, NULL);
	    errflag = rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO;
	    break;
	case LUA_TSTRING:
	    str = lua_tolstring(L, p, &len);
	    rc = SQLBindParameter(statement->stmt, i, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (SQLPOINTER)str, len, NULL);
	    errflag = rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO;
	    break;
	case LUA_TBOOLEAN:
	    buffer = realloc(buffer, offset + sizeof(int));
	    boolean = (int *)buffer + offset;
	    *boolean = lua_toboolean(L, p);
	    offset += sizeof(int);
	    rc = SQLBindParameter(statement->stmt, i, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, (SQLPOINTER)boolean, len, NULL);
	    errflag = rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO;
	    break;
	default:
	    /*
	     * Unknown/unsupported value type
	     */
	    errflag = 1;
            snprintf(err, sizeof(err)-1, DBI_ERR_BINDING_TYPE_ERR, lua_typename(L, type));
            errstr = err;
	}

	if (errflag)
	    break;
    }

    if (errflag) {
	realloc(buffer, 0);
	lua_pushboolean(L, 0);

	if (errstr) {
	    lua_pushfstring(L, DBI_ERR_BINDING_PARAMS, errstr);
	} else {
	    SQLGetDiagRec(SQL_HANDLE_STMT, statement->stmt, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

	    lua_pushfstring(L, DBI_ERR_BINDING_PARAMS, message);
	}
    
	return 2;
    }

    rc = SQLExecute(statement->stmt);
    if (rc != SQL_SUCCESS) {
	SQLGetDiagRec(SQL_HANDLE_STMT, statement->stmt, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

	lua_pushnil(L);
	lua_pushfstring(L, DBI_ERR_PREP_STATEMENT, message);
	return 2;
    }

    /* 
     * identify the number of output columns 
     */
    rc = SQLNumResultCols(statement->stmt, &statement->num_result_columns);

    if (statement->num_result_columns > 0) {
	resultset = (resultset_t *)malloc(sizeof(resultset_t) * statement->num_result_columns);
	memset(resultset, 0, sizeof(resultset_t) * statement->num_result_columns);

	bind = (bindparams_t *)malloc(sizeof(bindparams_t) * statement->num_result_columns);
	memset(bind, 0, sizeof(bindparams_t) * statement->num_result_columns);

	for (i = 0; i < statement->num_result_columns; i++) {
	    /* 
	     * return a set of attributes for a column 
	     */
	    rc = SQLDescribeCol(statement->stmt,
                        (SQLSMALLINT)(i + 1),
                        resultset[i].name,
                        sizeof(resultset[i].name),
                        &resultset[i].name_len,
                        &resultset[i].type,
                        &resultset[i].size,
                        &resultset[i].scale,
                        NULL);

	    if (rc != SQL_SUCCESS) {
		SQLGetDiagRec(SQL_HANDLE_STMT, statement->stmt, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

		lua_pushnil(L);
		lua_pushfstring(L, DBI_ERR_DESC_RESULT, message);
		return 2;
	    }

	    bind[i].buffer_len = resultset[i].size+1;

	    /* 
	     *allocate memory to bind a column 
	     */
	    bind[i].buffer = (SQLCHAR *)malloc((int)bind[i].buffer_len);

	    rc = SQLBindCol(statement->stmt,
                       (SQLSMALLINT)(i + 1),
                       SQL_C_CHAR,
                       bind[i].buffer,
                       bind[i].buffer_len,
                       &bind[i].len);

	    if (rc != SQL_SUCCESS) {
		SQLGetDiagRec(SQL_HANDLE_STMT, statement->stmt, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

		lua_pushnil(L);
		lua_pushfstring(L, DBI_ERR_ALLOC_RESULT, message);
		return 2;
	    }
	}

	statement->resultset = resultset;
	statement->bind = bind;
    }

    /*
     * free the buffer with a resize to 0
     */
    realloc(buffer, 0);

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * must be called after an execute
 */
static int statement_fetch_impl(lua_State *L, statement_t *statement, int named_columns) {
    int i;
    int d;

    SQLRETURN rc = SQL_SUCCESS;
 
    if (!statement->resultset || !statement->bind) {
	lua_pushnil(L);
	return 1;
    }

    /* fetch each row, and display */
    rc = SQLFetch(statement->stmt);
    if (rc == SQL_NO_DATA_FOUND) {
	lua_pushnil(L);
	return 1;
    }

    d = 1; 
    lua_newtable(L);
    for (i = 0; i < statement->num_result_columns; i++) {
	lua_push_type_t lua_push = db2_to_lua_push(statement->resultset[i].type, statement->bind[i].len);
	const char *name = strlower((char *)statement->resultset[i].name);
	double val;
	char *value = (char *)statement->bind[i].buffer;

	switch (lua_push) {
	case LUA_PUSH_NIL:
	    if (named_columns) {
		LUA_PUSH_ATTRIB_NIL(name);
	    } else {
		LUA_PUSH_ARRAY_NIL(d);
	    }
	    break;
	case LUA_PUSH_INTEGER:
            if (named_columns) {
                LUA_PUSH_ATTRIB_INT(name, atoi(value));
            } else {
                LUA_PUSH_ARRAY_INT(d, atoi(value));
            }
	    break;
	case LUA_PUSH_NUMBER:
	    val = strtod(value, NULL);

            if (named_columns) {
		LUA_PUSH_ATTRIB_FLOAT(name, val);
            } else {
		LUA_PUSH_ARRAY_FLOAT(d, val);
            }
	    break;
	case LUA_PUSH_BOOLEAN:
            if (named_columns) {
                LUA_PUSH_ATTRIB_BOOL(name, atoi(value));
            } else {
                LUA_PUSH_ARRAY_BOOL(d, atoi(value));
            }
            break;	    
	case LUA_PUSH_STRING:
	    if (named_columns) {
		LUA_PUSH_ATTRIB_STRING(name, value);
	    } else {
		LUA_PUSH_ARRAY_STRING(d, value);
	    }    
	    break;
	default:
	    luaL_error(L, DBI_ERR_UNKNOWN_PUSH);
	}
    }

    return 1;    
}


static int next_iterator(lua_State *L) {
    statement_t *statement = (statement_t *)luaL_checkudata(L, lua_upvalueindex(1), DBD_DB2_STATEMENT);
    int named_columns = lua_toboolean(L, lua_upvalueindex(2));

    return statement_fetch_impl(L, statement, named_columns);
}

/*
 * table = statement:fetch(named_indexes)
 */
static int statement_fetch(lua_State *L) {
    statement_t *statement = (statement_t *)luaL_checkudata(L, 1, DBD_DB2_STATEMENT);
    int named_columns = lua_toboolean(L, 2);

    return statement_fetch_impl(L, statement, named_columns);
}

/*
 * iterfunc = statement:rows(named_indexes)
 */
static int statement_rows(lua_State *L) {
    if (lua_gettop(L) == 1) {
        lua_pushvalue(L, 1);
        lua_pushboolean(L, 0);
    } else {
        lua_pushvalue(L, 1);
        lua_pushboolean(L, lua_toboolean(L, 2));
    }

    lua_pushcclosure(L, next_iterator, 2);
    return 1;
}

/*
 * __gc
 */
static int statement_gc(lua_State *L) {
    /* always free the handle */
    statement_close(L);

    return 0;
}

int dbd_db2_statement_create(lua_State *L, connection_t *conn, const char *sql_query) { 
    SQLRETURN rc = SQL_SUCCESS;
    statement_t *statement = NULL;
    SQLHANDLE stmt;

    SQLCHAR message[SQL_MAX_MESSAGE_LENGTH + 1];
    SQLCHAR sqlstate[SQL_SQLSTATE_SIZE + 1];
    SQLINTEGER sqlcode;
    SQLSMALLINT length;	

    rc = SQLAllocHandle(SQL_HANDLE_STMT, conn->db2, &stmt);
    if (rc != SQL_SUCCESS) {
	SQLGetDiagRec(SQL_HANDLE_DBC, conn->db2, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

        lua_pushnil(L);
        lua_pushfstring(L, DBI_ERR_ALLOC_STATEMENT, message);
        return 2;
    }

    /*
     * turn off deferred prepare
     * statements will be sent to the server at prepare timr,
     * and therefor we can catch errors then rather 
     * than at execute time
     */
    rc = SQLSetStmtAttr(stmt,SQL_ATTR_DEFERRED_PREPARE,(SQLPOINTER)SQL_DEFERRED_PREPARE_OFF,0);

    rc = SQLPrepare(stmt, (SQLCHAR *)sql_query, SQL_NTS);
    if (rc != SQL_SUCCESS) {
	SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length);

	lua_pushnil(L);
	lua_pushfstring(L, DBI_ERR_PREP_STATEMENT, message);
	return 2;
    }

    statement = (statement_t *)lua_newuserdata(L, sizeof(statement_t));
    statement->stmt = stmt;
    statement->db2 = conn->db2;
    statement->resultset = NULL;
    statement->bind = NULL;

    luaL_getmetatable(L, DBD_DB2_STATEMENT);
    lua_setmetatable(L, -2);

    return 1;
} 

int dbd_db2_statement(lua_State *L) {
    static const luaL_Reg statement_methods[] = {
	{"close", statement_close},
	{"execute", statement_execute},
	{"fetch", statement_fetch},
	{"rows", statement_rows},
	{NULL, NULL}
    };

    static const luaL_Reg statement_class_methods[] = {
	{NULL, NULL}
    };

    luaL_newmetatable(L, DBD_DB2_STATEMENT);
    luaL_register(L, 0, statement_methods);
    lua_pushvalue(L,-1);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, statement_gc);
    lua_setfield(L, -2, "__gc");

    luaL_register(L, DBD_DB2_STATEMENT, statement_class_methods);

    return 1;    
}