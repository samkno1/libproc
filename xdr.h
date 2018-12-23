#ifndef XDR_H
#define XDR_H

#include <stdio.h>
#include <stdint.h>
#include <polysat/proclib.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct XDR_FieldDefinition;

enum XDR_PRINT_STYLE { XDR_PRINT_HUMAN, XDR_PRINT_KVP, XDR_PRINT_CSV_HEADER,
   XDR_PRINT_CSV_DATA };

typedef int (*XDR_Decoder)(char *src, void *dst, size_t *inc, size_t max);
typedef int (*XDR_Encoder)(char *src, void *dst, size_t *inc, size_t max);
typedef void (*XDR_print_func)(FILE *out, void *data, void *arg,
      enum XDR_PRINT_STYLE style);
typedef void (*XDR_print_field_func)(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);

struct XDR_Union {
   uint32_t type;
   void *data;
};

struct XDR_FieldDefinition {
   XDR_Decoder decoder;
   XDR_Encoder encoder;
   size_t offset;
   const char *key;
   const char *name;
   const char *unit;
   double conv_offset, conv_divisor;
   XDR_print_field_func printer;
   void (*scanner)(const char *in, void *dst, void *arg);
   void (*dealloc)(void **, struct XDR_FieldDefinition *field);
   uint32_t struct_id;
   const char *description;
};

struct XDR_StructDefinition {
   uint32_t type;
   size_t in_memory_size;
   int (*encoder)(void *src, char *dst, size_t *inc, size_t max,
         uint32_t type, void *arg);
   int (*decoder)(char *src, void *dst, size_t *used, size_t max, void *arg);
   void *arg;
   void *(*allocator)(struct XDR_StructDefinition *def);
   void (*deallocator)(void **goner, struct XDR_StructDefinition *def);
   XDR_print_func print_func;
};

extern void XDR_register_structs(struct XDR_StructDefinition*);
extern void XDR_register_struct(struct XDR_StructDefinition*);
extern struct XDR_StructDefinition *XDR_definition_for_type(uint32_t type);

extern int XDR_struct_encoder(void *src, char *dst, size_t *encoded_size,
      size_t max, uint32_t type, void *arg);
extern void XDR_print_structure(uint32_t type,
      struct XDR_StructDefinition *str, char *buff, size_t len, void *arg1,
      int arg2);
extern void XDR_print_fields_func(FILE *out, void *data, void *arg,
      enum XDR_PRINT_STYLE style);


// All decoder functions take the exact same set of parameters to
//  permit more efficient, generic parsing code
//
// The parameters are:
//  char *src -- The address to start decoding from
//  void *dst -- The address to store the decoded data.  The structure of this 
//                memory will be type dependent
//  size_t *used -- Out parameter that returns the number of bytes used from
//                    the src buffer
//  size_t max -- The total number of bytes available in the src buffer
//
//  Returns negative number on error, or 0 on success.  Positive numbers are
//     reserved for future use
extern int XDR_struct_decoder(char *src, void *dst, size_t *used,
      size_t max, void *arg);
extern int XDR_decode_int32(char *src, int32_t *dst, size_t *used, size_t max);
extern int XDR_decode_uint32(char *src, uint32_t *dst, size_t *used,
      size_t max);
extern int XDR_decode_int64(char *src, int64_t *dst, size_t *used, size_t max);
extern int XDR_decode_uint64(char *src, uint64_t *dst, size_t *used,
      size_t max);
extern int XDR_decode_float(char *src, float *dst, size_t *used, size_t max);
extern int XDR_decode_double(char *src, double *dst, size_t *used, size_t max);
extern int XDR_decode_union(char *src, struct XDR_Union *dst, size_t *used, size_t max);
extern int XDR_decode_byte_string(char *src, struct XDR_Union *dst, size_t *used, size_t max);
extern int XDR_decode_string(char *src, char **dst, size_t *used,
      size_t max);

// Encode functions
extern int XDR_encode_uint32(uint32_t *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_int32(int32_t *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_int64(int64_t *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_uint64(uint64_t *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_union(struct XDR_Union *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_byte_string(char *src, char *dst,
      size_t *used, size_t max);
extern int XDR_encode_string(const char *src, char *dst,
      size_t *used, size_t max);


extern void *XDR_malloc_allocator(struct XDR_StructDefinition*);
extern void XDR_free_deallocator(void **goner, struct XDR_StructDefinition *);
extern void XDR_struct_free_deallocator(void **goner,
      struct XDR_StructDefinition *def);
extern void XDR_struct_field_deallocator(void **goner,
      struct XDR_FieldDefinition *field);
extern void XDR_free_union(struct XDR_Union *goner);

extern void XDR_print_field_int32(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);
extern void XDR_print_field_uint32(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);
extern void XDR_print_field_uint64(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);
extern void XDR_print_field_union(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);
extern void XDR_print_field_byte_string(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);
extern void XDR_print_field_string(FILE *out, void *data,
      struct XDR_FieldDefinition *field, enum XDR_PRINT_STYLE style);

extern void XDR_scan_int32(const char *in, void *dst, void *arg);
extern void XDR_scan_uint32(const char *in, void *dst, void *arg);
extern void XDR_scan_int64(const char *in, void *dst, void *arg);
extern void XDR_scan_uint64(const char *in, void *dst, void *arg);
extern void XDR_scan_string(const char *in, void *dst, void *arg);
extern void XDR_scan_bytes(const char *in, void *dst, void *arg);

#endif
