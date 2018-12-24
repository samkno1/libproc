/*
 * Copyright PolySat, California Polytechnic State University, San Luis Obispo. cubesat@calpoly.edu
 * This file is part of libproc, a PolySat library.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file cmd.c Command Handler Source
 *
 * Command handler library component implementation source file.
 *
 * @author Greg Manyak
 * @author John Bellardo
 */
#include <dlfcn.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include "config.h"
#include "proclib.h"
#include "ipc.h"
#include "cmd.h"
#include "debug.h"
#include "hashtable.h"
#include "xdr.h"
#include "cmd_schema.h"

static struct HashTable *xdrCommandHash = NULL;
static struct HashTable *xdrErrorHash = NULL;

/// Value in the PROT element in the CMD structure that indicates protected cmd
#define CMD_PROTECTED 1

// Code to handle multicast packet management
struct MulticastCommand {
   int cmdNum;
   MCAST_handler_t callback;
   void *callbackParam;

   struct MulticastCommand *next;
};

struct McastCommandState {
   struct in_addr srcAddr;
   uint16_t port;
   int fd;
   struct MulticastCommand *cmds;
   struct McastCommandState *next;
};

struct CMD_XDRCommandInfo *CMD_xdr_cmd_by_number(uint32_t num);

static int multicast_cmd_handler_cb(int socket, char type, void * arg)
{
   unsigned char data[MAX_IP_PACKET_SIZE];
   struct MulticastCommand *cmd = NULL;
   struct McastCommandState *state = (struct McastCommandState*)arg;
   size_t dataLen;
   struct sockaddr_in src;
   data[0] = 0;

   if (!state)
      return EVENT_KEEP;

   // should only be read events, but make sure
   if (type == EVENT_FD_READ) {
      // read from the socket to get the command and it's data
      dataLen = socket_read(socket, data, MAX_IP_PACKET_SIZE, &src);

      // make sure something was actually read
      if (dataLen > 0) {
         DBG_print(DBG_LEVEL_INFO, "MCast Received command 0x%02x", *data);

         for (cmd = state->cmds; cmd; cmd = cmd->next) {
            if (cmd->cmdNum < 0 || cmd->cmdNum == *data)
               cmd->callback(cmd->callbackParam, socket, *data, &data[1],
                  dataLen - 1, &src);
         }
      }
   }

   return EVENT_KEEP;
}

static struct McastCommandState *find_mcast_state(struct CommandCbArg *st,
   struct in_addr addr, uint16_t port)
{
    struct McastCommandState *curr;

    port = htons(port);
    for (curr = st->mcast; curr; curr = curr->next)
       if (curr->port == port && curr->srcAddr.s_addr == addr.s_addr)
          return curr;

    return NULL;
}

void cmd_set_multicast_handler(struct CommandCbArg *st,
   struct EventState *evt_loop, const char *service, int cmdNum,
   MCAST_handler_t handler, void *arg)
{
   struct MulticastCommand *cmd;
   struct in_addr addr = socket_multicast_addr_by_name(service);
   uint16_t port = socket_multicast_port_by_name(service);
   struct McastCommandState *state;
   struct ip_mreq mreq;

   if (!addr.s_addr || !port)
      return;

   state = find_mcast_state(st, addr, port);
   if (!state) {
      state = (struct McastCommandState*)malloc(sizeof(*state));
      memset(state, 0, sizeof(*state));

      state->srcAddr = addr;
      state->port = htons(port);
      state->fd = socket_init(port);
      if (state->fd <= 0) {
         free(state);
         return;
      }

      // Join multicast group
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      mreq.imr_multiaddr.s_addr = addr.s_addr;
      if (setsockopt(state->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
		 sizeof(struct ip_mreq)) == -1) {
         ERR_REPORT(DBG_LEVEL_WARN, "Failed to join multicast group for %s\n",
            service);
         return;
      }

      EVT_fd_add(evt_loop, state->fd, EVENT_FD_READ, multicast_cmd_handler_cb,
                     state);
      EVT_fd_set_name(evt_loop, state->fd, "Multicast Listener");
      state->next = st->mcast;
      st->mcast = state;
   }

   cmd = (struct MulticastCommand*)malloc(sizeof(*cmd));
   memset(cmd, 0, sizeof(*cmd));

   cmd->cmdNum = cmdNum;
   cmd->callback = handler;
   cmd->callbackParam = arg;

   cmd->next = state->cmds;
   state->cmds = cmd;
}

void cmd_remove_multicast_handler(struct CommandCbArg *st,
   const char *service, int cmdNum, struct EventState *evt_loop)
{
   struct MulticastCommand **itr, *cmd;
   struct McastCommandState **st_itr, *state;
   struct ip_mreq mreq;
   struct in_addr addr = socket_multicast_addr_by_name(service);
   uint16_t port = socket_multicast_port_by_name(service);

   state = find_mcast_state(st, addr, port);
   if (!state)
      return;

   // Remove all matching entries
   itr = &state->cmds;
   while (itr && *itr) {
      cmd = *itr;
      if ( cmd->cmdNum == cmdNum) {
         *itr = cmd->next;
         free(cmd);
         continue;
      }

      itr = &cmd->next;
   }

   // Clean up the socket if there are no more commands registered
   if (!state->cmds) {
      if (state->fd > 0) {
         EVT_fd_remove(evt_loop, state->fd, EVENT_FD_READ);

         mreq.imr_interface.s_addr = htonl(INADDR_ANY);
         mreq.imr_multiaddr.s_addr = addr.s_addr;
         setsockopt(state->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
	                             sizeof(struct ip_mreq));
         close(state->fd);
      }
      state->fd = 0;

      for (st_itr = &st->mcast; st_itr && *st_itr; ) {
         state = *st_itr;
         if (!state->fd) {
            *st_itr = state->next;
            free(state);
         }
         else
            st_itr = &state->next;
      }
   }
}

void cmd_cleanup_cb_state(struct CommandCbArg *st, struct EventState *evt_loop)
{
   struct McastCommandState *state;
   struct MulticastCommand *cmd;
   struct ip_mreq mreq;

   while ((state = st->mcast)) {
      while ((cmd = state->cmds)) {
         state->cmds = cmd->next;
         free(cmd);
      }

      if (state->fd > 0) {
         EVT_fd_remove(evt_loop, state->fd, EVENT_FD_READ);

         mreq.imr_interface.s_addr = htonl(INADDR_ANY);
         mreq.imr_multiaddr.s_addr = state->srcAddr.s_addr;
         setsockopt(state->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq,
	                             sizeof(struct ip_mreq));
         close(state->fd);
      }

      st->mcast = state->next;
      free(state);
   }
}

// Structure to hold a single command
struct CFG_CommandDesc {
   char *procName;
   char *cmdName;
   uint32_t cmdNum;
   char *funcName;
   CMD_handler_t cmdHandler;
};

static void CFG_CommandDesc_free(void *val)
{
   struct CFG_CommandDesc *curr = (struct CFG_CommandDesc *)val;
   if (!curr)
      return;

   if (curr->procName)
      free(curr->procName);

   if (curr->cmdName)
      free(curr->cmdName);

   if (curr->funcName)
      free(curr->funcName);

   free(curr);
}

// Create the CFG object that describes how to parse a single CommandDesc
CFG_NEWOBJ(CommandDesc,
      // Allocate a new instance of the struct
      CFG_MALLOC(struct CFG_CommandDesc),
      CFG_NULL,
      // PROC string value put into the structure with strdup
      CFG_STRDUP("PROC", struct CFG_CommandDesc, procName),
      // NAME string value put into the structure with strdup
      CFG_STRDUP("NAME", struct CFG_CommandDesc, cmdName),
      // FUNC string value put into the structure with strdup
      CFG_STRDUP("FUNC", struct CFG_CommandDesc, funcName),
      // NUM uint32 value put into the structure
      CFG_UINT32("NUM", struct CFG_CommandDesc, cmdNum)
);

// Declare the configuration root structure.  In this case it just contains
//  one array.
struct CFG_Root {
   struct CFG_Array cmds;
};

// Create the CFG object that describes how to parse the file root.
CFG_NEWOBJ(Root,
   // Allocate a new instance of the struct
   CFG_MALLOC(struct CFG_Root),
   CFG_NULL,
   // Only one option "CMD", appended to an array
   CFG_OBJARR("CMD", &CFG_OBJNAME(CommandDesc), struct CFG_Root, cmds)
);

void invalidCommand(int socket, unsigned char cmd, void * data, size_t dataLen,
                                                      struct sockaddr_in * src);

// Initialize the command handler
int cmd_handler_init(const char * procName, struct CommandCbArg *cmds)
{
   struct CFG_Root *root = NULL;
   int i;
   char cfgFile[256];

   if (procName) {
      sprintf(cfgFile, "./%s.cmd.cfg", procName);

      // Open the configuration file with the commands
      if (!CFG_locateConfigFile(cfgFile)) {
         DBG_print(DBG_LEVEL_WARN, "No command configuration file found\n");
         cmds->cmds = NULL;
         return EXIT_SUCCESS;
      }
      // Parse the commands
      root = (struct CFG_Root*) CFG_parseFile(&CFG_OBJNAME(Root));

      DBG_print(DBG_LEVEL_INFO, "%s found config file\n", procName);
   }

   cmds->cmds = (struct Command*)malloc(sizeof(struct Command)*MAX_NUM_CMDS);

   // Initialize array to the "invalid command" function
   for (i = 0; i < MAX_NUM_CMDS; i++) {
      (cmds->cmds + i)->cmd_cb = (CMD_handler_t)invalidCommand;
   }

   // Iterate through the commands
   for (i = 0; root && i < root->cmds.len; i++) {
      // Get the next object from the file
      struct CFG_CommandDesc *cmd =
         (struct CFG_CommandDesc*)root->cmds.data[i];

      // Translate the function name into a function pointer!
      cmd->cmdHandler = (CMD_handler_t)dlsym(RTLD_DEFAULT, cmd->funcName);

      // Check if it actually returned a valid pointer
      if (!cmd->cmdHandler) {
         DBG_print(DBG_LEVEL_WARN, "[%s command file parse error] %s\n",
               procName, dlerror());
      } else {
         DBG_print(DBG_LEVEL_INFO, "%s registered cmd %s [%d]\n", procName,
                                                cmd->funcName, cmd->cmdNum);

         // Add command and related params to array
         (cmds->cmds + cmd->cmdNum)->cmd_cb = cmd->cmdHandler;
      }
   }

   if (root) {
      CFG_freeArray(&root->cmds, &CFG_CommandDesc_free);
      free(root);
   }

   return EXIT_SUCCESS;
}

int cmd_handler_cb(int socket, char type, void * arg)
{
   unsigned char data[MAX_IP_PACKET_SIZE];
   struct Command *cmd = NULL;
   struct CommandCbArg *cmds = (struct CommandCbArg*)arg;
   size_t dataLen, used = 0;
   struct sockaddr_in src;
   struct IPC_Command xdr_cmd;
   struct CMD_XDRCommandInfo *cmd_info;
   data[0] = 0;

   // should only be read events, but make sure
   if (type == EVENT_FD_READ) {
      // read from the socket to get the command and it's data
      dataLen = socket_read(socket, data, MAX_IP_PACKET_SIZE, &src);

      // make sure something was actually read
      if (dataLen > 0) {
         // Command 0 was never used.  Now it is used to tell the difference
         //  between the old command format and the newer XDR format
         if (*data == 0) {
            if (IPC_Command_decode((char*)data, &xdr_cmd,
                     &used, dataLen, NULL) < 0) {
               DBG_print(DBG_LEVEL_WARN, "Failed to decode XDR command of "
                     "length %lu\n", dataLen);
            }
            else {
               cmd_info = CMD_xdr_cmd_by_number(xdr_cmd.cmd);
               if (cmd_info && cmd_info->handler)
                  cmd_info->handler(cmds->proc, &xdr_cmd, &src,
                        cmd_info->arg, socket);

               XDR_free_union(&xdr_cmd.parameters);
            }
         }
         else {
            cmd = cmds->cmds + *data;
            DBG_print(DBG_LEVEL_INFO, "Received command 0x%02x (%d - %d)",
                                       *data, cmd->uid, cmd->group);

            // Check to see if command is protected
            if (cmd->prot == CMD_PROTECTED) {
               //NOTE(Joshua Anderson): Cryptography support was reomved for now, so this is now a No-OP.
               DBG_print(DBG_LEVEL_WARN, "Protected commands are not supported\n");
            } else {
               // Un-protected command, nothing out of the ordinary here
               (*(cmd->cmd_cb))(socket, *data, data+1, dataLen-1, &src);
            }
         }
      }
   }

   return EVENT_KEEP;
}

int tx_cmd_handler_cb(int socket, char type, void * arg)
{
   unsigned char data[MAX_IP_PACKET_SIZE];
   // struct ProcessData *proc = (struct ProcessData*)arg;
   size_t dataLen;
   struct sockaddr_in src;
   data[0] = 0;

   // should only be read events, but make sure
   if (type == EVENT_FD_READ) {
      // read from the socket to get the command and it's data
      dataLen = socket_read(socket, data, MAX_IP_PACKET_SIZE, &src);

      // make sure something was actually read
      if (dataLen > 0) {
         DBG_print(DBG_LEVEL_INFO, "Received TX command response 0x%02x", data[0]);
      }
   }

   return EVENT_KEEP;
}

void invalidCommand(int socket, unsigned char cmd, void * data, size_t dataLen, struct sockaddr_in * src)
{
   // tell the source of the command they messed up
   //socket_write(socket, "Invalid command", strlen("Invalid command"), src);
   DBG_print(DBG_LEVEL_INFO, "Received invalid command: 0x%02x\n", cmd);
}

void cmd_handler_cleanup(struct CommandCbArg *cmds)
{
   if (cmds && cmds->cmds) {
      free(cmds->cmds);
   }
}

int CMD_iterate_structs(char *src, size_t len, CMD_struct_itr itr_cb, void *arg,
      int arg2)
{
   uint32_t type, arr_ents, byte_len;
   size_t used = 0;
   uint32_t arr_itr;
   struct XDR_StructDefinition *def;

   if (XDR_decode_uint32(src, &type, &used, len, NULL) < 0)
      return -1;

   src += used;
   len -= used;

   if (type != IPC_TYPES_OPAQUE_STRUCT_ARR) {
      def = XDR_definition_for_type(type);
      itr_cb(type, def, src, len, arg, arg2);
      return 0;
   }

   if (XDR_decode_uint32(src, &arr_ents, &used, len, NULL) < 0)
      return -1;

   src += used;
   len -= used;

   for (arr_itr = 0; arr_itr < arr_ents; arr_itr++) {
      if (XDR_decode_uint32(src, &byte_len, &used, len, NULL) < 0)
         return -2;

      src += used;
      len -= used;
      if (CMD_iterate_structs(src, byte_len, itr_cb, arg, arg2) < 0)
         return -3;

      src += byte_len;
      len -= byte_len;
   }

   return 0;
}

struct CMD_MulticallInfo *CMD_mc_cmd_by_name(const char *name,
      struct CMD_MulticallInfo *mc)
{
   struct CMD_MulticallInfo *curr;

   for (curr = mc; curr && curr->func; curr++)
      if (curr->name && 0 == strcasecmp(curr->name, name))
         return curr;

   return NULL;
}

struct CMD_HashByName_Args {
   const char *name;
   struct CMD_XDRCommandInfo *result;
};

static int cmd_hash_by_name(void *data, void *arg)
{
   struct CMD_HashByName_Args *p = (struct CMD_HashByName_Args*)arg;
   struct CMD_XDRCommandInfo *cmd = (struct CMD_XDRCommandInfo*)data;

   if (cmd->name && 0 == strcasecmp(cmd->name, p->name))
      p->result = cmd;

   return 0;
}

struct CMD_XDRCommandInfo *CMD_xdr_cmd_by_name(const char *name)
{
   struct CMD_HashByName_Args p = { name, NULL };
   HASH_iterate_arg_table(xdrCommandHash, &cmd_hash_by_name, &p);
   return p.result;
}

struct CMD_XDRCommandInfo *CMD_xdr_cmd_by_number(uint32_t num)
{
   return (struct CMD_XDRCommandInfo *)
      HASH_find_key(xdrCommandHash, (void*)(intptr_t)num);
}

int CMD_xdr_cmd_help(struct CMD_XDRCommandInfo *command)
{
   struct XDR_FieldDefinition *field, *fields = NULL;

   if (!command)
      return 2;

   printf("%s [-h <destination>]", command->name);
   if (command->parameter) {
      if (command->parameter->decoder != &XDR_struct_decoder ||
            command->parameter->encoder != &XDR_struct_encoder)
         return -1;
      fields = (struct XDR_FieldDefinition *)command->parameter->arg;

      for (field = fields; field && field->encoder; field++)
         if (field->key && field->scanner)
            printf(" [%s=<value>]", field->key);
   }
   printf("\n");

   printf(" %s\n", command->summary);
   printf("   destination -- DNS name or IP address of machine to receive the command\n");
   printf("   Valid parameter/value pairs are:\n");

   for (field = fields; field && field->encoder; field++)
      if (field->key && field->scanner) {
         if (field->description)
            printf("     %24s -- %s\n", field->key, field->description);
         else
            printf("     %24s -- UNDOCUMENTED\n", field->key);
      }

   return 2;
}

int CMD_mc_cmd_help(struct CMD_MulticallInfo *command)
{
   if (command) {
      printf("%s [-h <destination>] %s\n", command->name,
            command->help_param_summary);
      printf("%s\n%s\n", command->help_description, command->help_detail);
   }

   return 2;
}

static int print_cmd_summary(void *data)
{
   struct CMD_XDRCommandInfo *xdr = (struct CMD_XDRCommandInfo*)data;
   if (xdr->name && xdr->summary)
      printf("  \033[31m\033[1m%24s\033[0m -- %s\n", xdr->name, xdr->summary);
   else if (xdr->name)
      printf("  \033[31m\033[1m%24s\033[0m -- UNDOCUMENTED\n", xdr->name);
   return 0;
}


int CMD_usage_summary(struct CMD_MulticallInfo *mc, const char *name)
{
   printf("Usage: %s -c <command name>\n  Use --help with a command for detailed parameter information.\n\nAvailable commands are:\n", name);
   for (; mc && mc->func; mc++) {
      if (mc->name && mc->help_description)
         printf("  \033[31m\033[1m%24s\033[0m -- %s\n", mc->name, mc->help_description);
      else if (mc->name)
         printf("  \033[31m\033[1m%24s\033[0m -- UNDOCUMENTED\n", mc->name);
   }
   HASH_iterate_table(xdrCommandHash, &print_cmd_summary);

   return 1;
}

int CMD_send_command_line_command(int argc, char **argv,
      struct CMD_MulticallInfo *mc, ProcessData *proc, IPC_command_callback cb,
      void *cb_arg, unsigned int timeout, const char *destProc)
{
   char *host = "127.0.0.1";
   struct CMD_MulticallInfo *mcCommand = NULL;
   struct CMD_XDRCommandInfo *command = NULL;
   int argItr = 1;
   struct XDR_FieldDefinition *field, *fields = NULL;
   const char *execName = NULL;
   void *param = NULL;
   struct sockaddr_in dest;
   char *key, *value;
   int res;
   uint32_t param_type = 0;

   // Match command based on executable name
   execName = rindex(argv[0], '/');
   if (!execName)
      execName = argv[0];
   else
      execName++;

   command = CMD_xdr_cmd_by_name(execName);
   mcCommand = CMD_mc_cmd_by_name(execName, mc);

   // Process command line flags: -c, -h, -n, --help
   for (argItr = 1; argItr < argc && argv[argItr][0] == '-'; argItr++) {
      switch(argv[argItr][1]) {
         case 'c':
            if (argv[argItr][2] || argItr == (argc - 1))
               return CMD_usage_summary(mc, execName);
            command = CMD_xdr_cmd_by_name(argv[++argItr]);
            mcCommand = CMD_mc_cmd_by_name(argv[argItr], mc);
            break;

         case 'h':
            if (argv[argItr][2] || argItr == (argc - 1))
               return CMD_usage_summary(mc,  execName);
            host = argv[++argItr];
            break;

         case 'n':
            if (argv[argItr][2] || argItr == (argc - 1))
               return CMD_usage_summary(mc, execName);
            command = CMD_xdr_cmd_by_number(strtol(argv[++argItr], NULL, 0));
            break;

         case '-':
         default:
            if (!command && !mcCommand)
               return CMD_usage_summary(mc, execName);
            if (mcCommand)
               return CMD_mc_cmd_help(mcCommand);
            return CMD_xdr_cmd_help(command);
      }
   }

   if (mcCommand && mcCommand->func)
      return mcCommand->func(mcCommand, execName, argc - argItr,
            argv + argItr, host);
   if (!command)
      return CMD_usage_summary(mc, execName);

   // Resolve hostname
   dest.sin_family = AF_INET;
   dest.sin_addr.s_addr = 0;
   if (host)
      if (0 == socket_resolve_host(host, &dest.sin_addr))
         return -1;
   if (0 == dest.sin_addr.s_addr)
      inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
   dest.sin_port = htons(socket_get_addr_by_name(destProc));

   // Parse KVP parameters
   if (command->parameter) {
      if (command->parameter->decoder != &XDR_struct_decoder ||
            command->parameter->encoder != &XDR_struct_encoder ||
            !command->parameter->allocator || !command->parameter->deallocator)
         return -2;
      fields = (struct XDR_FieldDefinition *)command->parameter->arg;
      param = command->parameter->allocator(command->parameter);
      if (!param)
         return -3;

      for (; argItr < argc; argItr++) {
         key = argv[argItr];
         value = strchr(key, '=');
         if (!value) {
            command->parameter->deallocator(&param, command->parameter);
            return CMD_xdr_cmd_help(command);
         }

         *value++ = 0;
         param_type = command->params;
         for (field = fields; field->encoder; field++) {
            if (strcasecmp(field->key, key) || !field->scanner)
               continue;
            key = NULL;
            field->scanner(value, (char*)param + field->offset,
                  command->parameter->arg, (char*)param + field->len_offset);
            break;
         }
         if (key) {
            command->parameter->deallocator(&param, command->parameter);
            return CMD_xdr_cmd_help(command);
         }
      }
   }

   // Send command and print response
   res = IPC_command(proc, command->command, param, param_type, dest, cb, cb_arg,
         IPC_CB_TYPE_RAW, timeout);
   if (param)
      command->parameter->deallocator(&param, command->parameter);

   return res;
}

void CMD_register_commands(struct CMD_XDRCommandInfo *cmd, int override)
{
   for(; cmd && cmd->command > 0; cmd++)
      CMD_register_command(cmd, override);
}

static size_t xdr_cmd_hash_func(void *key)
{
   return (uintptr_t)key;
}

static void *xdr_cmd_key_for_data(void *data)
{
   if (!data)
      return 0;
   return (void*)(uintptr_t)(((struct CMD_XDRCommandInfo*)data)->command);
}

static int xdr_cmd_cmp_key(void *key1, void *key2)
{
   if ( ((uintptr_t)key1) == ((uintptr_t)key2) )
      return 1;
   return 0;
}

void CMD_register_command(struct CMD_XDRCommandInfo *cmd, int override)
{
   if (!cmd)
      return;

   if (!xdrCommandHash) {
      xdrCommandHash = HASH_create_table(37, &xdr_cmd_hash_func,
            &xdr_cmd_cmp_key, &xdr_cmd_key_for_data);
      if (!xdrCommandHash)
         return;
   }

   if (cmd->params)
      cmd->parameter = XDR_definition_for_type(cmd->params);

   if (HASH_find_key(xdrCommandHash, (void*)(intptr_t)cmd->command)) {
      if (override) {
         HASH_remove_key(xdrCommandHash, (void*)(intptr_t)cmd->command);
         HASH_add_data(xdrCommandHash, cmd);
      }
   }
   else
      HASH_add_data(xdrCommandHash, cmd);
}

void CMD_register_errors(struct CMD_ErrorInfo *errs)
{
   for(; errs && errs->id > 0; errs++)
      CMD_register_error(errs);
}

static size_t xdr_err_hash_func(void *key)
{
   return (uintptr_t)key;
}

static void *xdr_err_key_for_data(void *data)
{
   if (!data)
      return 0;
   return (void*)(uintptr_t)(((struct CMD_ErrorInfo*)data)->id);
}

static int xdr_err_cmp_key(void *key1, void *key2)
{
   if ( ((uintptr_t)key1) == ((uintptr_t)key2) )
      return 1;
   return 0;
}

void CMD_register_error(struct CMD_ErrorInfo *err)
{
   if (!err)
      return;

   if (!xdrErrorHash) {
      xdrErrorHash = HASH_create_table(37, &xdr_err_hash_func,
            &xdr_err_cmp_key, &xdr_err_key_for_data);
      if (!xdrErrorHash)
         return;
   }
   HASH_add_data(xdrErrorHash, err);
}

const char *CMD_error_message(uint32_t id)
{
   struct CMD_ErrorInfo *err;

   if (xdrErrorHash) {
      err = HASH_find_key(xdrErrorHash, (void*)(intptr_t)id);
      if (err && err->description)
         return err->description;
      if (err && err->name)
         return err->name;
   }

   return "";
}

void CMD_set_xdr_cmd_handler(uint32_t num, CMD_XDR_handler_t cb, void *arg)
{
   struct CMD_XDRCommandInfo *cmd;
   
   cmd = CMD_xdr_cmd_by_number(num);
   if (!cmd)
      return;

   cmd->handler = cb;
   cmd->arg = arg;
}

int CMD_resolve_callback(ProcessData *proc, IPC_command_callback cb,
      void *arg, enum IPC_CB_TYPE cb_type, void *rxbuff, size_t rxlen)
{
   if (!cb)
      return 0;

   if (cb_type == IPC_CB_TYPE_RAW) {
      cb(proc, 0, arg, rxbuff, rxlen, cb_type);
      return 0;
   }

   assert(cb_type == IPC_CB_TYPE_COOKED);

   return 0;
}

void CMD_print_response(struct ProcessData *proc, int timeout,
      void *arg, char *resp_buff, size_t resp_len, enum IPC_CB_TYPE cb_type)
{
   struct IPC_ResponseHeader hdr;
   size_t len = 0;

   if (cb_type == IPC_CB_TYPE_RAW) {
      if (IPC_ResponseHeader_decode(resp_buff, &hdr, &len, resp_len, NULL) < 0)
         return;

      if (hdr.result != IPC_RESULTCODE_SUCCESS) {
         printf("Error: %s\n", CMD_error_message(hdr.result));
      }
      else {
         CMD_iterate_structs(resp_buff + len, resp_len - len,
               &XDR_print_structure, stdout, XDR_PRINT_HUMAN);
      }
   }
   else if (cb_type == IPC_CB_TYPE_COOKED) {
   }
}

struct IPC_OpaqueStruct CMD_struct_to_opaque_struct(void *src, uint32_t type)
{
   struct IPC_OpaqueStruct result;
   struct XDR_StructDefinition *def;
   size_t dlen = 256, needed = 0;

   result.length = 0;
   result.data = NULL;
   def = XDR_definition_for_type(type);

   if (!src || !def || !def->encoder)
      return result;

   result.data = malloc(dlen);
   if (def->encoder(src, result.data, &needed, dlen, type, def->arg) < 0) {
      free(result.data);
      result.data = NULL;
      if (dlen >= needed)
         return result;

      dlen = needed + 16;
      result.data = malloc(dlen);
      needed = 0;
      if (def->encoder(src, result.data, &needed, dlen, type, def->arg) < 0) {
         free(result.data);
         result.data = NULL;
         return result;
      }
   }

   result.length = needed;
   return result;
}
