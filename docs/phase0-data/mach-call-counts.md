| Subsystem | Call | boot | shell | build |
|---|---|---:|---:|---:|
| device_pager | `device_pager_init_pager` | 1 | 0 | 0 |
| gnumach | `vm_get_size_limit` | 3 | 23 | 2302 |
| gnumach | `gsync_wait` | 1 | 13 | 1 |
| gnumach | `gsync_wake` | 0 | 12 | 0 |
| gnumach | `thread_set_name` | 2 | 0 | 0 |
| mach | `vm_deallocate` | 49 | 241 | 55580 |
| mach | `vm_map` | 137 | 381 | 54240 |
| mach | `vm_protect` | 57 | 213 | 30138 |
| mach | `vm_allocate` | 30 | 171 | 11957 |
| mach | `thread_resume` | 18 | 74 | 11421 |
| mach | `task_info` | 0 | 0 | 9243 |
| mach | `thread_set_state` | 17 | 59 | 7265 |
| mach | `thread_create` | 17 | 53 | 7264 |
| mach | `task_set_special_port` | 7 | 61 | 7085 |
| mach | `thread_get_state` | 16 | 50 | 4785 |
| mach | `thread_set_special_port` | 4 | 38 | 4783 |
| mach | `task_create` | 2 | 15 | 3064 |
| mach | `task_terminate` | 0 | 17 | 3059 |
| mach | `thread_abort` | 1 | 21 | 2482 |
| mach | `task_threads` | 1 | 15 | 2481 |
| mach | `task_get_special_port` | 4 | 23 | 2302 |
| mach | `vm_region` | 3 | 23 | 2302 |
| mach | `thread_suspend` | 0 | 13 | 1 |
| mach | `vm_statistics` | 0 | 1 | 4 |
| mach_host | `task_priority` | 1 | 0 | 0 |
| mach_i386 | `i386_set_gdt` | 19 | 63 | 7848 |
| mach_i386 | `i386_get_gdt` | 2 | 40 | 3288 |
| mach_port | `mach_port_deallocate` | 141 | 1464 | 343547 |
| mach_port | `mach_port_insert_right` | 41 | 341 | 57434 |
| mach_port | `mach_port_get_refs` | 14 | 234 | 45745 |
| mach_port | `mach_port_mod_refs` | 30 | 139 | 26600 |
| mach_port | `mach_port_allocate_name` | 8 | 121 | 20686 |
| mach_port | `mach_port_allocate` | 36 | 145 | 16473 |
| mach_port | `mach_port_set_qlimit` | 22 | 84 | 9387 |
| mach_port | `mach_port_extract_right` | 3 | 45 | 7443 |
| mach_port | `mach_port_destroy` | 0 | 1 | 5803 |
| mach_port | `mach_port_names` | 2 | 29 | 4199 |
| mach_port | `mach_port_move_member` | 3 | 0 | 7 |
| mach_port | `mach_port_set_protected_payload` | 3 | 0 | 0 |
| mach_port | `mach_port_request_notification` | 2 | 0 | 0 |

Totals: 697, 4223, 770219

Defined but not observed (159):
- device: device_close, device_get_status, device_intr_ack, device_intr_register, device_map, device_open, device_open_new, device_read, device_read_inband, device_set_filter, device_set_status, device_write, device_write_inband
- device_pager: device_pager_change_completed, device_pager_copy, device_pager_data_request, device_pager_data_return, device_pager_data_unlock, device_pager_lock_completed, device_pager_supply_completed, device_pager_terminate
- device_reply: device_open_reply, device_read_reply, device_read_reply_inband, device_write_reply, device_write_reply_inband
- exc: exception_raise
- gnumach: gsync_requeue, register_new_task_notification, task_set_essential, task_set_name, thread_get_name, thread_terminate_release, vm_allocate_contiguous, vm_cache_statistics, vm_msync, vm_object_sync, vm_pages_phys, vm_set_size_limit, vm_wire_all
- mach: mach_ports_lookup, mach_ports_register, memory_object_change_attributes, memory_object_data_error, memory_object_data_supply, memory_object_data_unavailable, memory_object_destroy, memory_object_get_attributes, memory_object_lock_request, memory_object_ready, task_get_emulation_vector, task_ras_control, task_resume, task_set_emulation, task_set_emulation_vector, task_suspend, thread_get_special_port, thread_info, thread_terminate, vm_copy, vm_inherit, vm_machine_attribute, vm_read, vm_set_default_memory_manager, vm_write
- mach4: memory_object_create_proxy, task_disable_pc_sampling, task_enable_pc_sampling, task_get_sampled_pcs, thread_disable_pc_sampling, thread_enable_pc_sampling, thread_get_sampled_pcs, vm_region_create_proxy
- mach_debug: host_ipc_marequest_info, host_slab_info, host_stack_usage, host_virtual_physical_table_info, mach_port_dnrequest_info, mach_port_get_srights, mach_port_kernel_object, mach_vm_object_info, mach_vm_object_pages, mach_vm_object_pages_phys, mach_vm_region_info, processor_set_stack_usage
- mach_host: host_adjust_time, host_adjust_time64, host_get_kernel_version, host_get_time, host_get_time64, host_get_uptime64, host_info, host_kernel_version, host_processor_set_priv, host_processor_sets, host_processors, host_reboot, host_set_time, host_set_time64, processor_assign, processor_control, processor_exit, processor_get_assignment, processor_info, processor_set_create, processor_set_default, processor_set_destroy, processor_set_info, processor_set_max_priority, processor_set_policy_disable, processor_set_policy_enable, processor_set_processors, processor_set_tasks, processor_set_threads, processor_start, task_assign, task_assign_default, task_get_assignment, task_max_priority, thread_assign, thread_assign_default, thread_depress_abort, thread_get_assignment, thread_max_priority, thread_policy, thread_priority, thread_wire, vm_wire
- mach_i386: i386_get_ldt, i386_get_xstate_size, i386_io_perm_create, i386_io_perm_modify, i386_set_ldt
- mach_port: mach_port_clear_protected_payload, mach_port_get_receive_status, mach_port_get_set_status, mach_port_rename, mach_port_set_ktype, mach_port_set_mscount, mach_port_set_seqno, mach_port_type
- memory_object: memory_object_change_completed, memory_object_copy, memory_object_data_request, memory_object_data_return, memory_object_data_unlock, memory_object_init, memory_object_lock_completed, memory_object_supply_completed, memory_object_terminate
- memory_object_default: memory_object_create, memory_object_data_initialize
- notify: mach_notify_dead_name, mach_notify_msg_accepted, mach_notify_no_senders, mach_notify_port_deleted, mach_notify_port_destroyed, mach_notify_send_once
- task_notify: mach_notify_new_task
