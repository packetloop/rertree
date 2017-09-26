#include <stdlib.h> /*malloc, free, NULL, exit*/

#include "rtree_impl.h"
#include "redismodule.h"

#define RTMODULE_NAME "RTREE"
#define RTREE_VERSION 00001
#define RT_ENCODING_VERSION 1

static RedisModuleType *RtreeType;

typedef enum { RT_OK = 0, RT_MISSING, RT_EMPTY, RT_MISMATCH } lookupStatus;

static const char *statusStrerror(int status) {
    switch (status) {
        case RT_MISSING:
        case RT_EMPTY:
            return "ERR not found";
        case RT_MISMATCH:
            return REDISMODULE_ERRORMSG_WRONGTYPE;
        case RT_OK:
            return "ERR item exists";
        default:
            return "Unknown error";
    }
}

static int rtGetValue(RedisModuleKey *key, RTreePtr **pRtree) {
    *pRtree = NULL;
    if (key == NULL) {
        return RT_MISSING;
    }
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        return RT_EMPTY;
    } else if (type == REDISMODULE_KEYTYPE_MODULE && RedisModule_ModuleTypeGetType(key) == RtreeType) {
        *pRtree = RedisModule_ModuleTypeGetValue(key);
        return RT_OK;
    } else {
        return RT_MISMATCH;
    }
}

static bool load_rt_node(RedisModuleCtx *ctx, struct RTNodeList *node, RedisModuleString **argv) {
  void *newval = malloc(sizeof(long long));
  
  if (RedisModule_StringToLongLong(argv[0], &node->I[0]) != REDISMODULE_OK)
    return REDISMODULE_ERR;
  if (RedisModule_StringToLongLong(argv[1], &node->I[1])   != REDISMODULE_OK)
    return REDISMODULE_ERR;
  if (!newval || (RedisModule_StringToLongLong(argv[2], (long long*)newval) != REDISMODULE_OK)) //TODO load VAL strings too
    return REDISMODULE_ERR;

  node->Next = NULL;
  node->Tuple = newval;
  
  return REDISMODULE_OK;
}

static void FreeNodeList(struct RTNodeList *list) {
  struct RTNodeList *list_save = list;
  
  while (list) {
    list_save = list->Next;
    free(list);
    list = list_save;
  }
}

// rtree.add {key} [{start} {end} {val}]+
static int RtAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);
  // arity checks
  if (argc < 5) {
    RedisModule_WrongArity(ctx);
    return REDISMODULE_ERR;
  }

  if ( (argc - 2) % 3 ) {
    RedisModule_WrongArity(ctx);
    return REDISMODULE_ERR;
  }

  RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
  RTreePtr *prtree = NULL;
  int status = rtGetValue(key, &prtree);
  struct RTNodeList node;
  node.Next = NULL;

  if (status == RT_EMPTY || status == RT_MISSING) {//TODO rtree.create dim
    if (load_rt_node(ctx, &node, &argv[2]) == REDISMODULE_ERR)
      return RedisModule_ReplyWithError(ctx, "ERR could not load value");
    prtree = malloc(sizeof(RTreePtr));//TODO error handling
    RTNewTree(prtree, &node);
    if (*prtree == NULL)
      return RedisModule_ReplyWithError(ctx, "ERR could not create rtree");
    RedisModule_ModuleTypeSetValue(key, RtreeType, prtree);
  } else if (status == RT_OK) { // rtree already exist
    if (load_rt_node(ctx, &node, &argv[2]) == REDISMODULE_ERR)
      return RedisModule_ReplyWithError(ctx, "ERR could not load value");
    RTInsertTuple(prtree, node.I, node.Tuple);
  } else { // error
    return RedisModule_ReplyWithError(ctx, statusStrerror(status));
  }

  // load extra values
  for (size_t ii = 5; ii < argc; ii += 3) { // TODO proper fail for multiple values
    if (load_rt_node(ctx, &node, &argv[ii]) == REDISMODULE_ERR)
      return RedisModule_ReplyWithError(ctx, "ERR could not load value");
    RTInsertTuple(prtree, node.I, node.Tuple);
  }

  RedisModule_ReplyWithSimpleString(ctx, "OK");
  return REDISMODULE_OK;
}

// rtree.find {key} {start} {stop} - NOTE - it searches in [] interval
static int RtFind_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);
  // arity checks
  if (argc != 4) {
    RedisModule_WrongArity(ctx);
    return REDISMODULE_ERR;
  }

  RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
  RTreePtr *prtree = NULL;
  int status = rtGetValue(key, &prtree);
  if (status != RT_OK) {
    return RedisModule_ReplyWithError(ctx, statusStrerror(status));
  }

  size_t count;
  struct RTNodeList *list, *list_save;
  RTdimension I[RTn*2];

  long long start, end;
  if (RedisModule_StringToLongLong(argv[2], &start) != REDISMODULE_OK)
    return RedisModule_ReplyWithError(ctx, "ERR could not load start value");
  if (RedisModule_StringToLongLong(argv[3], &end)   != REDISMODULE_OK)
    return RedisModule_ReplyWithError(ctx, "ERR could not load end value");

  I[0] = start;
  I[1] = end;
  RTSelectTuple(prtree, I, &list, &count);
  list_save = list;

  RedisModule_ReplyWithArray(ctx, count);
  for (size_t ii = 0; ii < count; ++ii) { //TODO check if count(list) is different
    RedisModule_ReplyWithLongLong(ctx, *((long long *)list->Tuple));
    list = list->Next;
  }
  
  // free search list
  //FreeNodeList(list_save);//TODO free

  return REDISMODULE_OK;
}

static void NodeRdbLoad(RedisModuleIO *io, struct RTNodeList *pnode) {
  pnode->Next = NULL;
  pnode->I[0] = RedisModule_LoadUnsigned(io);
  pnode->I[1] = RedisModule_LoadUnsigned(io);
  
  long long *val = malloc(sizeof(long long));
  *val = RedisModule_LoadUnsigned(io);
  pnode->Tuple = val;
}

static void *RtRdbLoad(RedisModuleIO *io, int encver) {
  if (encver > RT_ENCODING_VERSION) {
    return NULL;
  }

  RTreePtr *prtree = malloc(sizeof(RTreePtr));
  *prtree = NULL;
  struct RTNodeList node;
  size_t count = RedisModule_LoadUnsigned(io);

  if (count > 0) {
    NodeRdbLoad(io, &node);
    RTNewTree(prtree, &node);
  }
  
  for (size_t i = 1; i < count; ++i) {
    NodeRdbLoad(io, &node);
    RTInsertTuple(prtree, node.I, node.Tuple);
  }

  return prtree;
}

static void RtRdbSave(RedisModuleIO *io, void *obj) { 
  RTreePtr *prtree = obj;
  struct RTNodeList *list, *list_save;
  size_t count;
  RTdimension I[RTn*2];
  
  RTSelectDimensions(prtree, I);
  RTSelectTuple(prtree, I, &list, &count); // will use LOTS of memory with prod size, need to optimize
  list_save = list;

  RedisModule_SaveUnsigned(io, count);
  for (size_t i = 0; i < count; ++i) {
    RedisModule_SaveUnsigned(io, list->I[0]);
    RedisModule_SaveUnsigned(io, list->I[1]);
    RedisModule_SaveUnsigned(io, *((long long*)list->Tuple));
    list = list->Next;
  }

  //FreeNodeList(list_save);//TODO free
}

static void RtAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
  RTreePtr *prtree = value;
  struct RTNodeList *list, *list_save;
  size_t count;
  RTdimension I[RTn*2];
  
  RTSelectDimensions(prtree, I);
  RTSelectTuple(prtree, I, &list, &count); // will use LOTS of memory with prod size, need to optimize
  list_save = list;
  
  for (size_t ii = 0; ii < count; ++ii) { //TODO check if count(list) is different
    RedisModule_EmitAOF(aof, "rtree.add", "slb", key, list->I[0], list->I[1], *((long long*)list->Tuple));
    list = list->Next;
  }
  
  // free search list
  FreeNodeList(list_save);
}

static void RtFree(void *value) { RTFreeTree(value); }

static size_t RtMemUsage(const void *value) {
    //TODO exact size
    RTdimension I[RTn*2];
    RTreePtr prtree = (RTreePtr)value;
    RTSelectDimensions(&prtree, I);
    size_t count;
    RTSelectTuple(&prtree, I, NULL, &count);

    return (count * (sizeof(struct RTNodeList) + sizeof(long long)));
}

/* Unit test entry point for the module. */
int TestModule(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);

  RedisModule_ReplyWithSimpleString(ctx, "PASS");
  return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx) {
  if (RedisModule_Init(ctx, RTMODULE_NAME, RTREE_VERSION, REDISMODULE_APIVER_1) ==
      REDISMODULE_ERR)
    return REDISMODULE_ERR;
  
  /* Main commands. */
  if (RedisModule_CreateCommand(ctx, "rtree.add", RtAdd_RedisCommand,
                                "write deny-oom fast", 1, 1,
                                1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;
  if (RedisModule_CreateCommand(ctx, "rtree.find", RtFind_RedisCommand,
                                "readonly", 1, 1, 1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  static RedisModuleTypeMethods typeprocs = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                                               .rdb_load = RtRdbLoad,
                                               .rdb_save = RtRdbSave,
                                               .aof_rewrite = RtAofRewrite,
                                               .free = RtFree,
                                               .mem_usage = RtMemUsage};// acc. to code not implemented?
                                               
  RtreeType = RedisModule_CreateDataType(ctx, "ReRtree--", RT_ENCODING_VERSION, &typeprocs);

  return ((RtreeType == NULL) ? REDISMODULE_ERR : REDISMODULE_OK);
}
