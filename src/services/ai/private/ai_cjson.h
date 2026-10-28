#define AI_CJSON_HIDE_SYMBOLS
#define AI_CJSON_NESTING_LIMIT 32
/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef ai_cjson__h
#define ai_cjson__h

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

/* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called from a project with a different default calling convention.  For windows you have 3 define options:

AI_CJSON_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
AI_CJSON_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
AI_CJSON_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

For *nix builds that support visibility attribute, you can define similar behavior by

setting default visibility to hidden by adding
-fvisibility=hidden (for gcc)
or
-xldscope=hidden (for sun cc)
to CFLAGS

then using the AI_CJSON_API_VISIBILITY flag to "export" the same symbols the way AI_CJSON_EXPORT_SYMBOLS does

*/

#define AI_CJSON_CDECL __cdecl
#define AI_CJSON_STDCALL __stdcall

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(AI_CJSON_HIDE_SYMBOLS) && !defined(AI_CJSON_IMPORT_SYMBOLS) && !defined(AI_CJSON_EXPORT_SYMBOLS)
#define AI_CJSON_EXPORT_SYMBOLS
#endif

#if defined(AI_CJSON_HIDE_SYMBOLS)
#define AI_CJSON_PUBLIC(type)   type AI_CJSON_STDCALL
#elif defined(AI_CJSON_EXPORT_SYMBOLS)
#define AI_CJSON_PUBLIC(type)   __declspec(dllexport) type AI_CJSON_STDCALL
#elif defined(AI_CJSON_IMPORT_SYMBOLS)
#define AI_CJSON_PUBLIC(type)   __declspec(dllimport) type AI_CJSON_STDCALL
#endif
#else /* !__WINDOWS__ */
#define AI_CJSON_CDECL
#define AI_CJSON_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(AI_CJSON_API_VISIBILITY)
#define AI_CJSON_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define AI_CJSON_PUBLIC(type) type
#endif
#endif

/* project version */
#define AI_CJSON_VERSION_MAJOR 1
#define AI_CJSON_VERSION_MINOR 7
#define AI_CJSON_VERSION_PATCH 17

#include <stddef.h>

/* Private entry points require the current thread to hold the JSON lock. */
int ai_json_locked(void);
int ai_json_preflight(const char *text, size_t size);
int ai_json_utf8(const char *text, size_t size);
/* Error category of the last parse in the current locked transaction. */
int ai_cjson_GetParseError(void);

/* ai_cjson Types: */
#define ai_cjson_Invalid (0)
#define ai_cjson_False  (1 << 0)
#define ai_cjson_True   (1 << 1)
#define ai_cjson_NULL   (1 << 2)
#define ai_cjson_Number (1 << 3)
#define ai_cjson_String (1 << 4)
#define ai_cjson_Array  (1 << 5)
#define ai_cjson_Object (1 << 6)
#define ai_cjson_Raw    (1 << 7) /* raw json */

#define ai_cjson_IsReference 256
#define ai_cjson_StringIsConst 512

/* The ai_cjson structure: */
typedef struct ai_cjson
{
    /* next/prev allow you to walk array/object chains. Alternatively, use GetArraySize/GetArrayItem/GetObjectItem */
    struct ai_cjson *next;
    struct ai_cjson *prev;
    /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */
    struct ai_cjson *child;

    /* The type of the item, as above. */
    int type;

    /* The item's string, if type==ai_cjson_String  and type == ai_cjson_Raw */
    char *valuestring;
    /* writing to valueint is DEPRECATED, use ai_cjson_SetNumberValue instead */
    int valueint;
    /* The item's number, if type==ai_cjson_Number */
    double valuedouble;

    /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
    char *string;
} ai_cjson;

typedef int ai_cjson_bool;

/* Limits how deeply nested arrays/objects can be before ai_cjson rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef AI_CJSON_NESTING_LIMIT
#define AI_CJSON_NESTING_LIMIT 1000
#endif

/* returns the version of ai_cjson as a string */
AI_CJSON_PUBLIC(const char*) ai_cjson_Version(void);

/* All allocations use ai_alloc/free. Delete parsed trees with
 * ai_cjson_Delete and printed strings with ai_cjson_free or ai_free. */
/* Supply a block of JSON, and this returns a ai_cjson object you can interrogate. */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_Parse(const char *value);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_ParseWithLength(const char *value, size_t buffer_length);
/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to the final byte parsed. */
/* If you supply a ptr in return_parse_end and parsing fails, then return_parse_end will contain a pointer to the error so will match ai_cjson_GetErrorPtr(). */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_ParseWithOpts(const char *value, const char **return_parse_end, ai_cjson_bool require_null_terminated);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, ai_cjson_bool require_null_terminated);

/* Render a ai_cjson entity to text for transfer/storage. */
AI_CJSON_PUBLIC(char *) ai_cjson_Print(const ai_cjson *item);

AI_CJSON_PUBLIC(void) ai_cjson_DeleteChar(char *buffer);

/* Render a ai_cjson entity to text for transfer/storage without any formatting. */
AI_CJSON_PUBLIC(char *) ai_cjson_PrintUnformatted(const ai_cjson *item);
/* Render a ai_cjson entity to text using a buffered strategy. prebuffer is a guess at the final size. guessing well reduces reallocation. fmt=0 gives unformatted, =1 gives formatted */
AI_CJSON_PUBLIC(char *) ai_cjson_PrintBuffered(const ai_cjson *item, int prebuffer, ai_cjson_bool fmt);
/* Render a ai_cjson entity to text using a buffer already allocated in memory with given length. Returns 1 on success and 0 on failure. */
/* NOTE: ai_cjson is not always 100% accurate in estimating how much memory it will use, so to be safe allocate 5 bytes more than you actually need */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_PrintPreallocated(ai_cjson *item, char *buffer, const int length, const ai_cjson_bool format);
/* Delete a ai_cjson entity and all subentities. */
AI_CJSON_PUBLIC(void) ai_cjson_Delete(ai_cjson *item);

/* Returns the number of items in an array (or object). */
AI_CJSON_PUBLIC(int) ai_cjson_GetArraySize(const ai_cjson *array);
/* Retrieve item number "index" from array "array". Returns NULL if unsuccessful. */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_GetArrayItem(const ai_cjson *array, int index);
/* Get item "string" from object. Case insensitive. */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_GetObjectItem(const ai_cjson * const object, const char * const string);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_GetObjectItemCaseSensitive(const ai_cjson * const object, const char * const string);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_HasObjectItem(const ai_cjson *object, const char *string);
/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to look a few chars back to make sense of it. Defined when ai_cjson_Parse() returns 0. 0 when ai_cjson_Parse() succeeds. */
AI_CJSON_PUBLIC(const char *) ai_cjson_GetErrorPtr(void);

/* Check item type and return its value */
AI_CJSON_PUBLIC(char *) ai_cjson_GetStringValue(const ai_cjson * const item);
AI_CJSON_PUBLIC(double) ai_cjson_GetNumberValue(const ai_cjson * const item);

/* These functions check the type of an item */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsInvalid(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsFalse(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsTrue(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsBool(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsNull(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsNumber(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsString(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsArray(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsObject(const ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_IsRaw(const ai_cjson * const item);

/* These calls create a ai_cjson item of the appropriate type. */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateNull(void);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateTrue(void);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateFalse(void);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateBool(ai_cjson_bool boolean);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateNumber(double num);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateString(const char *string);
/* raw json */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateRaw(const char *raw);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateArray(void);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateObject(void);

/* Create a string where valuestring references a string so
 * it will not be freed by ai_cjson_Delete */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateStringReference(const char *string);
/* Create an object/array that only references it's elements so
 * they will not be freed by ai_cjson_Delete */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateObjectReference(const ai_cjson *child);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateArrayReference(const ai_cjson *child);

/* These utilities create an Array of count items.
 * The parameter count cannot be greater than the number of elements in the number array, otherwise array access will be out of bounds.*/
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateIntArray(const int *numbers, int count);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateFloatArray(const float *numbers, int count);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateDoubleArray(const double *numbers, int count);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_CreateStringArray(const char *const *strings, int count);

/* Append item to the specified array/object. */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_AddItemToArray(ai_cjson *array, ai_cjson *item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_AddItemToObject(ai_cjson *object, const char *string, ai_cjson *item);
/* Use this when string is definitely const (i.e. a literal, or as good as), and will definitely survive the ai_cjson object.
 * WARNING: When this function was used, make sure to always check that (item->type & ai_cjson_StringIsConst) is zero before
 * writing to `item->string` */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_AddItemToObjectCS(ai_cjson *object, const char *string, ai_cjson *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing ai_cjson to a new ai_cjson, but don't want to corrupt your existing ai_cjson. */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_AddItemReferenceToArray(ai_cjson *array, ai_cjson *item);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_AddItemReferenceToObject(ai_cjson *object, const char *string, ai_cjson *item);

/* Remove/Detach items from Arrays/Objects. */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_DetachItemViaPointer(ai_cjson *parent, ai_cjson * const item);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_DetachItemFromArray(ai_cjson *array, int which);
AI_CJSON_PUBLIC(void) ai_cjson_DeleteItemFromArray(ai_cjson *array, int which);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_DetachItemFromObject(ai_cjson *object, const char *string);
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_DetachItemFromObjectCaseSensitive(ai_cjson *object, const char *string);
AI_CJSON_PUBLIC(void) ai_cjson_DeleteItemFromObject(ai_cjson *object, const char *string);
AI_CJSON_PUBLIC(void) ai_cjson_DeleteItemFromObjectCaseSensitive(ai_cjson *object, const char *string);

/* Update array items. */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_InsertItemInArray(ai_cjson *array, int which, ai_cjson *newitem); /* Shifts pre-existing items to the right. */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_ReplaceItemViaPointer(ai_cjson * const parent, ai_cjson * const item, ai_cjson * replacement);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_ReplaceItemInArray(ai_cjson *array, int which, ai_cjson *newitem);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_ReplaceItemInObject(ai_cjson *object,const char *string,ai_cjson *newitem);
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_ReplaceItemInObjectCaseSensitive(ai_cjson *object,const char *string,ai_cjson *newitem);

/* Duplicate a ai_cjson item */
AI_CJSON_PUBLIC(ai_cjson *) ai_cjson_Duplicate(const ai_cjson *item, ai_cjson_bool recurse);
/* Duplicate will create a new, identical ai_cjson item to the one you pass, in new memory that will
 * need to be released. With recurse!=0, it will duplicate any children connected to the item.
 * The item->next and ->prev pointers are always zero on return from Duplicate. */
/* Recursively compare two ai_cjson items for equality. If either a or b is NULL or invalid, they will be considered unequal.
 * case_sensitive determines if object keys are treated case sensitive (1) or case insensitive (0) */
AI_CJSON_PUBLIC(ai_cjson_bool) ai_cjson_Compare(const ai_cjson * const a, const ai_cjson * const b, const ai_cjson_bool case_sensitive);

/* Minify a strings, remove blank characters(such as ' ', '\t', '\r', '\n') from strings.
 * The input pointer json cannot point to a read-only address area, such as a string constant,
 * but should point to a readable and writable address area. */
AI_CJSON_PUBLIC(void) ai_cjson_Minify(char *json);

/* Helper functions for creating and adding items to an object at the same time.
 * They return the added item or NULL on failure. */
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddNullToObject(ai_cjson * const object, const char * const name);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddTrueToObject(ai_cjson * const object, const char * const name);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddFalseToObject(ai_cjson * const object, const char * const name);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddBoolToObject(ai_cjson * const object, const char * const name, const ai_cjson_bool boolean);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddNumberToObject(ai_cjson * const object, const char * const name, const double number);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddStringToObject(ai_cjson * const object, const char * const name, const char * const string);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddRawToObject(ai_cjson * const object, const char * const name, const char * const raw);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddObjectToObject(ai_cjson * const object, const char * const name);
AI_CJSON_PUBLIC(ai_cjson*) ai_cjson_AddArrayToObject(ai_cjson * const object, const char * const name);

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define ai_cjson_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* helper for the ai_cjson_SetNumberValue macro */
AI_CJSON_PUBLIC(double) ai_cjson_SetNumberHelper(ai_cjson *object, double number);
#define ai_cjson_SetNumberValue(object, number) ((object != NULL) ? ai_cjson_SetNumberHelper(object, (double)number) : (number))
/* Change the valuestring of a ai_cjson_String object, only takes effect when type of object is ai_cjson_String */
AI_CJSON_PUBLIC(char*) ai_cjson_SetValuestring(ai_cjson *object, const char *valuestring);

/* If the object is not a boolean type this does nothing and returns ai_cjson_Invalid else it returns the new type*/
#define ai_cjson_SetBoolValue(object, boolValue) ( \
    (object != NULL && ((object)->type & (ai_cjson_False|ai_cjson_True))) ? \
    (object)->type=((object)->type &(~(ai_cjson_False|ai_cjson_True)))|((boolValue)?ai_cjson_True:ai_cjson_False) : \
    ai_cjson_Invalid\
)

/* Macro for iterating over an array or object */
#define ai_cjson_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/* Allocate/free objects through the fixed application allocator. */
AI_CJSON_PUBLIC(void *) ai_cjson_malloc(size_t size);
AI_CJSON_PUBLIC(void) ai_cjson_free(void *object);

#ifdef __cplusplus
}
#endif

#endif
