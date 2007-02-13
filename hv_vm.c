/*
 * Cisco router simulation platform.
 * Copyright (c) 2006 Christophe Fillot (cf@utc.fr)
 *
 * Hypervisor generic VM routines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "cpu.h"
#include "vm.h"
#include "dynamips.h"
#include "device.h"
#include "dev_c7200.h"
#include "dev_vtty.h"
#include "utils.h"
#include "base64.h"
#include "net.h"
#include "atm.h"
#include "frame_relay.h"
#include "crc.h"
#include "net_io.h"
#include "net_io_bridge.h"
#ifdef GEN_ETH
#include "gen_eth.h"
#endif
#include "registry.h"
#include "hypervisor.h"

/* Find the specified CPU */
static cpu_gen_t *find_cpu(hypervisor_conn_t *conn,vm_instance_t *vm,
                           u_int cpu_id)
{
   cpu_gen_t *cpu;

   cpu = cpu_group_find_id(vm->cpu_group,cpu_id);

   if (!cpu) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BAD_OBJ,1,"Bad CPU specified");
      return NULL;
   }
   
   return cpu;
}

/* Set debugging level */
static int cmd_set_debug_level(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->debug_level = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"O",argv[0]);
   return(0);
}

/* Set IOS image filename */
static int cmd_set_ios(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (vm_ios_set_image(vm,argv[1]) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                            "unable to store IOS image name for router '%s'",
                            argv[0]);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"IOS image set for '%s'",argv[0]);
   return(0);
}

/* Set IOS configuration filename to load at startup */
static int cmd_set_config(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (vm_ios_set_config(vm,argv[1]) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                            "unable to store IOS config for router '%s'",
                            argv[0]);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"IOS config file set for '%s'",
                         argv[0]);
   return(0);
}

/* Set RAM size */
static int cmd_set_ram(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->ram_size = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set NVRAM size */
static int cmd_set_nvram(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->nvram_size = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Enable/disable use of a memory-mapped file to simulate RAM */
static int cmd_set_ram_mmap(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->ram_mmap = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Enable/disable use of sparse memory */
static int cmd_set_sparse_mem(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->sparse_mem = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the clock divisor */
static int cmd_set_clock_divisor(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   u_int clock_div;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if ((clock_div = atoi(argv[1])) != 0)
      vm->clock_divisor = clock_div;

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the idle PC */
static int cmd_set_idle_pc(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->idle_pc = strtoull(argv[1],NULL,0);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the idle PC value when the CPU is online */
static int cmd_set_idle_pc_online(hypervisor_conn_t *conn,
                                  int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   cpu->set_idle_pc(cpu,strtoull(argv[2],NULL,0));

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Get the idle PC proposals */
static int cmd_get_idle_pc_prop(hypervisor_conn_t *conn,int argc,char *argv[])
{  
   vm_instance_t *vm;
   cpu_gen_t *cpu;
   int i;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   cpu->get_idling_pc(cpu);

   for(i=0;i<cpu->idle_pc_prop_count;i++) {
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"0x%llx [%d]",
                            cpu->idle_pc_prop[i].pc,
                            cpu->idle_pc_prop[i].count);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Dump the idle PC proposals */
static int cmd_show_idle_pc_prop(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;
   int i;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   for(i=0;i<cpu->idle_pc_prop_count;i++) {
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"0x%llx [%d]",
                            cpu->idle_pc_prop[i].pc,
                            cpu->idle_pc_prop[i].count);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set CPU idle max value */
static int cmd_set_idle_max(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   cpu->idle_max = atoi(argv[2]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set CPU idle sleep time value */
static int cmd_set_idle_sleep_time(hypervisor_conn_t *conn,
                                   int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   cpu->idle_sleep_time = atoi(argv[2]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show info about potential timer drift */
static int cmd_show_timer_drift(hypervisor_conn_t *conn,
                                int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!(cpu = find_cpu(conn,vm,atoi(argv[1]))))
      return(-1);

   if (cpu->type == CPU_TYPE_MIPS64) {
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"Timer Drift: %u",
                            CPU_MIPS64(cpu)->timer_drift);

      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"Pending Timer IRQ: %u",
                            CPU_MIPS64(cpu)->timer_irq_pending);      
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the exec area size */
static int cmd_set_exec_area(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->exec_area_size = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set ghost RAM file */
static int cmd_set_ghost_file(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->ghost_ram_filename = strdup(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set ghost RAM status */
static int cmd_set_ghost_status(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->ghost_status = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}


/* Set PCMCIA ATA disk0 size */
static int cmd_set_disk0(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->pcmcia_disk_size[0] = atoi(argv[1]);
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set PCMCIA ATA disk1 size */
static int cmd_set_disk1(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->pcmcia_disk_size[1] = atoi(argv[1]);
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the config register used at startup */
static int cmd_set_conf_reg(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->conf_reg_setup = strtol(argv[1],NULL,0);
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set TCP port for console */
static int cmd_set_con_tcp_port(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->vtty_con_type = VTTY_TYPE_TCP;
   vm->vtty_con_tcp_port = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set TCP port for AUX port */
static int cmd_set_aux_tcp_port(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm->vtty_aux_type = VTTY_TYPE_TCP;
   vm->vtty_aux_tcp_port = atoi(argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Read an IOS configuration file from a given router */
static int cmd_extract_config(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   char *cfg_buffer,*cfg_base64;
   ssize_t cfg_len;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!vm->nvram_extract_config)
      goto err_no_extract_method;

   /* Extract the IOS configuration */
   if (((cfg_len = vm->nvram_extract_config(vm,&cfg_buffer)) < 0) ||
       (cfg_buffer == NULL)) 
      goto err_nvram_extract;

   /* 
    * Convert config to base64. base64 is about 1/3 larger than input,
    * let's be on the safe side with twice longer.
    */
   if (!(cfg_base64 = malloc(cfg_len * 2)))
      goto err_alloc_base64;

   base64_encode(cfg_base64,cfg_buffer,cfg_len);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"conf '%s' %s",argv[0],cfg_base64);

   free(cfg_buffer);
   free(cfg_base64);
   return(0);

 err_alloc_base64:
   free(cfg_buffer);
 err_nvram_extract:
 err_no_extract_method:
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                         "unable to extract config of VM '%s'",argv[0]);
   return(-1);
}

/* Push an IOS configuration file */
static int cmd_push_config(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   char *cfg_buffer;
   ssize_t len;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   if (!vm->nvram_push_config)
      goto err_no_push_method;

   /* Convert base64 input to standard text */
   if (!(cfg_buffer = malloc(3 * strlen(argv[1]))))
      goto err_alloc_base64;

   if ((len = base64_decode(cfg_buffer,argv[1],0)) < 0)
      goto err_decode_base64;

   /* Push configuration */
   if (vm->nvram_push_config(vm,cfg_buffer,len) < 0)
      goto err_nvram_push;

   free(cfg_buffer);
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,
                         "IOS config file pushed tm VM '%s'",
                         argv[0]);
   return(0);

 err_nvram_push:
 err_decode_base64:
   free(cfg_buffer);
 err_alloc_base64:
 err_no_push_method:
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                         "unable to push IOS config for VM '%s'",
                         argv[0]);
   return(-1);
}

/* Show info about the specified CPU */
static int cmd_show_cpu_info(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   cpu_gen_t *cpu;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   cpu = cpu_group_find_id(vm->cpu_group,atoi(argv[1]));

   if (cpu) {
      cpu->reg_dump(cpu);
      cpu->mmu_dump(cpu);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Suspend a VM instance */
static int cmd_suspend(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm_suspend(vm);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"VM '%s' suspended",argv[0]);
   return(0);
}

/* Resume a VM instance */
static int cmd_resume(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vm_resume(vm);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"VM '%s' resumed",argv[0]);
   return(0);
}

/* Send a message on the console */
static int cmd_send_con_msg(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vtty_store_str(vm->vtty_con,argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Send a message on the AUX port */
static int cmd_send_aux_msg(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_object(conn,argv[0],OBJ_TYPE_VM)))
      return(-1);

   vtty_store_str(vm->vtty_aux,argv[1]);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show info about VM object */
static void cmd_show_vm_list(registry_entry_t *entry,void *opt,int *err)
{
   hypervisor_conn_t *conn = opt;
   vm_instance_t *vm = entry->data;

   hypervisor_send_reply(conn,HSC_INFO_MSG,0,"%s (%s)",
                         entry->name,vm_get_type(vm));
}

/* VM List */
static int cmd_vm_list(hypervisor_conn_t *conn,int argc,char *argv[])
{
   int err = 0;
   registry_foreach_type(OBJ_TYPE_VM,cmd_show_vm_list,conn,&err);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show console TCP port info about VM object */
static void cmd_show_vm_list_con_ports(registry_entry_t *entry,void *opt,
                                       int *err)
{
   hypervisor_conn_t *conn = opt;
   vm_instance_t *vm = entry->data;

   if (vm->vtty_con_type == VTTY_TYPE_TCP)
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"%s (%d)",
                            vm->name,vm->vtty_con_tcp_port);
}

/* VM console TCP port list */
static int cmd_vm_list_con_ports(hypervisor_conn_t *conn,int argc,char *argv[])
{
   int err = 0;
   registry_foreach_type(OBJ_TYPE_VM,cmd_show_vm_list_con_ports,conn,&err);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* VM commands */
static hypervisor_cmd_t vm_cmd_array[] = {
   { "set_debug_level", 2, 2, cmd_set_debug_level, NULL },
   { "set_ios", 2, 2, cmd_set_ios, NULL },
   { "set_config", 2, 2, cmd_set_config, NULL },
   { "set_ram", 2, 2, cmd_set_ram, NULL },
   { "set_nvram", 2, 2, cmd_set_nvram, NULL },
   { "set_ram_mmap", 2, 2, cmd_set_ram_mmap, NULL },
   { "set_sparse_mem", 2, 2, cmd_set_sparse_mem, NULL },
   { "set_clock_divisor", 2, 2, cmd_set_clock_divisor, NULL },
   { "set_exec_area", 2, 2, cmd_set_exec_area, NULL },
   { "set_disk0", 2, 2, cmd_set_disk0, NULL },
   { "set_disk1", 2, 2, cmd_set_disk1, NULL },
   { "set_conf_reg", 2, 2, cmd_set_conf_reg, NULL },
   { "set_idle_pc", 2, 2, cmd_set_idle_pc, NULL },
   { "set_idle_pc_online", 3, 3, cmd_set_idle_pc_online, NULL },
   { "get_idle_pc_prop", 2, 2, cmd_get_idle_pc_prop, NULL },
   { "show_idle_pc_prop", 2, 2, cmd_show_idle_pc_prop, NULL },
   { "set_idle_max", 3, 3, cmd_set_idle_max, NULL },
   { "set_idle_sleep_time", 3, 3, cmd_set_idle_sleep_time, NULL },
   { "show_timer_drift", 2, 2, cmd_show_timer_drift, NULL },
   { "set_ghost_file", 2, 2, cmd_set_ghost_file, NULL },
   { "set_ghost_status", 2, 2, cmd_set_ghost_status, NULL },
   { "set_con_tcp_port", 2, 2, cmd_set_con_tcp_port, NULL },
   { "set_aux_tcp_port", 2, 2, cmd_set_aux_tcp_port, NULL },
   { "extract_config", 1, 1, cmd_extract_config, NULL },
   { "push_config", 2, 2, cmd_push_config, NULL },
   { "cpu_info", 2, 2, cmd_show_cpu_info, NULL },
   { "suspend", 1, 1, cmd_suspend, NULL },
   { "resume", 1, 1, cmd_resume, NULL },
   { "send_con_msg", 2, 2, cmd_send_con_msg, NULL },
   { "send_aux_msg", 2, 2, cmd_send_aux_msg, NULL },
   { "list", 0, 0, cmd_vm_list, NULL },
   { "list_con_ports", 0, 0, cmd_vm_list_con_ports, NULL },
   { NULL, -1, -1, NULL, NULL },
};

/* Hypervisor VM initialization */
int hypervisor_vm_init(void)
{
   hypervisor_module_t *module;

   module = hypervisor_register_module("vm");
   assert(module != NULL);

   hypervisor_register_cmd_array(module,vm_cmd_array);
   return(0);
}