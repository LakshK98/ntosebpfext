// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

#include "ebpf_ext.h"
#include "ebpf_ext_hook_provider.h"
#include "ebpf_ext_tracelog.h"
#include "ebpf_extension_uuids.h"

typedef struct _ebpf_ext_hook_client_rundown
{
    EX_RUNDOWN_REF protection;
    bool rundown_occurred;
} ebpf_ext_hook_client_rundown_t;

struct _ebpf_extension_hook_provider;

/**
 * @brief Data structure representing a hook NPI client (attached eBPF program). This is returned
 * as the provider binding context in the NMR client attach callback.
 */
typedef struct _ebpf_extension_hook_client
{
    LIST_ENTRY link;                               ///< Link to next client (if any).
    HANDLE nmr_binding_handle;                     ///< NMR binding handle.
    GUID client_module_id;                         ///< NMR module Id.
    const void* client_binding_context;            ///< Client supplied context to be passed when invoking eBPF program.
    const ebpf_extension_data_t* client_data;      ///< Client supplied attach parameters.
    ebpf_program_invoke_function_t invoke_program; ///< Pointer to function to invoke eBPF program.
    void* provider_data; ///< Opaque pointer to hook specific data associated with this client.
    struct _ebpf_extension_hook_provider* provider_context; ///< Pointer to the hook NPI provider context.
    PIO_WORKITEM detach_work_item;          ///< Pointer to IO work item that is invoked to detach the client.
    ebpf_ext_hook_client_rundown_t rundown; ///< Pointer to rundown object used to synchronize detach operation.
} ebpf_extension_hook_client_t;

typedef struct _ebpf_extension_hook_clients_list
{
    EX_PUSH_LOCK lock;
    LIST_ENTRY attached_clients_list;
} ebpf_extension_hook_clients_list_t;

typedef struct _ebpf_extension_hook_provider
{
    NPI_PROVIDER_CHARACTERISTICS characteristics;         ///< NPI Provider characteristics.
    HANDLE nmr_provider_handle;                           ///< NMR binding handle.
    EX_SPIN_LOCK lock;                                    ///< Lock for synchronization.
    ebpf_extension_hook_on_client_attach attach_callback; /*!< Pointer to hook specific callback to be invoked
                                                              when a client attaches. */
    ebpf_extension_hook_on_client_detach detach_callback; /*!< Pointer to hook specific callback to be invoked
                                                              when a client detaches. */
    const void* custom_data; ///< Opaque pointer to hook specific data associated for this provider.
    _Guarded_by_(lock)
        LIST_ENTRY attached_clients_list; ///< Linked list of hook NPI clients that are attached to this provider.
} ebpf_extension_hook_provider_t;

/**
 * @brief Initialize the hook client rundown state.
 *
 * @param[in, out] hook_client Pointer to the attached hook NPI client.
 *
 * @retval STATUS_SUCCESS Operation succeeded.
 * @retval STATUS_INSUFFICIENT_RESOURCES IO work item could not be allocated.
 */
static NTSTATUS
_ebpf_ext_attach_init_rundown(ebpf_extension_hook_client_t* hook_client)
{
    NTSTATUS status = STATUS_SUCCESS;
    ebpf_ext_hook_client_rundown_t* rundown = &hook_client->rundown;

    EBPF_EXT_LOG_ENTRY();

    //
    // Allocate work item for client detach processing.
    //
    hook_client->detach_work_item = IoAllocateWorkItem(_ebpf_ext_driver_device_object);
    if (hook_client->detach_work_item == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        EBPF_EXT_LOG_NTSTATUS_API_FAILURE(EBPF_EXT_TRACELOG_KEYWORD_EXTENSION, "IoAllocateWorkItem", status);
        goto Exit;
    }

    // Initialize the rundown and disable new references.
    ExInitializeRundownProtection(&rundown->protection);
    rundown->rundown_occurred = FALSE;

Exit:
    EBPF_EXT_RETURN_NTSTATUS(status);
}

/**
 * @brief Block execution of the thread until all invocations are completed.
 *
 * @param[in, out] rundown Rundown object to wait for.
 *
 */
static void
_ebpf_ext_attach_wait_for_rundown(_Inout_ ebpf_ext_hook_client_rundown_t* rundown)
{
    EBPF_EXT_LOG_ENTRY();

    ExWaitForRundownProtectionRelease(&rundown->protection);
    rundown->rundown_occurred = TRUE;

    EBPF_EXT_LOG_EXIT();
}

IO_WORKITEM_ROUTINE _ebpf_extension_detach_client_completion;
#if !defined(__cplusplus)
#pragma alloc_text(PAGE, _ebpf_extension_detach_client_completion)
#endif

/**
 * @brief IO work item routine callback that waits on client rundown to complete.
 *
 * @param[in] device_object IO Device object.
 * @param[in] context Pointer to work item context.
 *
 */
void
_ebpf_extension_detach_client_completion(_In_ DEVICE_OBJECT* device_object, _In_opt_ void* context)
{
    ebpf_extension_hook_client_t* hook_client = (ebpf_extension_hook_client_t*)context;
    PIO_WORKITEM work_item;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(device_object);

    EBPF_EXT_LOG_ENTRY();

    ASSERT(hook_client != NULL);
    _Analysis_assume_(hook_client != NULL);

    work_item = hook_client->detach_work_item;

    // The NMR model is async, but the only Windows run-down protection API available is a blocking API, so the
    // following call will block until all using threads are complete. This should be fixed in the future.
    // Issue: https://github.com/microsoft/ebpf-for-windows/issues/1854

    // Wait for any in progress callbacks to complete.
    _ebpf_ext_attach_wait_for_rundown(&hook_client->rundown);

    IoFreeWorkItem(work_item);

    // Note: This frees the provider binding context (hook_client).
    NmrProviderDetachClientComplete(hook_client->nmr_binding_handle);

    EBPF_EXT_LOG_EXIT();
}

_Must_inspect_result_ bool
ebpf_extension_hook_client_enter_rundown(_Inout_ ebpf_extension_hook_client_t* hook_client)
{
    ebpf_ext_hook_client_rundown_t* rundown = &hook_client->rundown;
    bool status = ExAcquireRundownProtection(&rundown->protection);
    return status;
}

void
ebpf_extension_hook_client_leave_rundown(_Inout_ ebpf_extension_hook_client_t* hook_client)
{
    ebpf_ext_hook_client_rundown_t* rundown = &hook_client->rundown;
    ExReleaseRundownProtection(&rundown->protection);
}

const ebpf_extension_data_t*
ebpf_extension_hook_client_get_client_data(_In_ const ebpf_extension_hook_client_t* hook_client)
{
    return hook_client->client_data;
}

const GUID*
ebpf_extension_hook_provider_get_client_module_id(_In_ const ebpf_extension_hook_client_t* client_context)
{
    return &client_context->client_module_id;
}

void
ebpf_extension_hook_client_set_provider_data(_In_ ebpf_extension_hook_client_t* hook_client, const void* data)
{
    hook_client->provider_data = (void*)data;
}

const void*
ebpf_extension_hook_client_get_provider_data(_In_ const ebpf_extension_hook_client_t* hook_client)
{
    return hook_client->provider_data;
}

const void*
ebpf_extension_hook_provider_get_custom_data(_In_ const ebpf_extension_hook_provider_t* provider_context)
{
    return provider_context->custom_data;
}

_Must_inspect_result_ ebpf_result_t
ebpf_extension_hook_invoke_program(
    _In_ const ebpf_extension_hook_client_t* client, _Inout_ void* context, _Out_ uint32_t* result)
{
    ebpf_program_invoke_function_t invoke_program = client->invoke_program;
    const void* client_binding_context = client->client_binding_context;

    ebpf_result_t invoke_result = invoke_program(client_binding_context, context, result);
    EBPF_EXT_RETURN_RESULT(invoke_result);
}

_Must_inspect_result_ ebpf_result_t
ebpf_extension_hook_check_attach_parameter(
    size_t attach_parameter_size,
    _In_reads_(attach_parameter_size) const void* attach_parameter,
    _In_reads_(attach_parameter_size) const void* wild_card_attach_parameter,
    _Inout_ ebpf_extension_hook_provider_t* provider_context)
{
    ebpf_result_t result = EBPF_SUCCESS;
    bool using_wild_card_attach_parameter = FALSE;
    bool lock_held = FALSE;

    EBPF_EXT_LOG_ENTRY();

    if (memcmp(attach_parameter, wild_card_attach_parameter, attach_parameter_size) == 0) {
        using_wild_card_attach_parameter = TRUE;
    }

    KIRQL oldIrql = ExAcquireSpinLockShared(&provider_context->lock);
    lock_held = TRUE;
    if (using_wild_card_attach_parameter) {
        // Client requested wild card attach parameter. This will only be allowed if there are no other clients
        // attached.
        if (!IsListEmpty(&provider_context->attached_clients_list)) {
            EBPF_EXT_LOG_MESSAGE(
                EBPF_EXT_TRACELOG_LEVEL_ERROR,
                EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
                "Wildcard attach denied as other clients present.");
            result = EBPF_ACCESS_DENIED;
            goto Exit;
        }
    } else {
        // Ensure there are no other clients with wild card attach parameter or with the same attach parameter as the
        // requesting client.

        LIST_ENTRY* link = provider_context->attached_clients_list.Flink;
        while (link != &provider_context->attached_clients_list) {
            ebpf_extension_hook_client_t* next_client =
                (ebpf_extension_hook_client_t*)CONTAINING_RECORD(link, ebpf_extension_hook_client_t, link);

            const ebpf_extension_data_t* next_client_data = next_client->client_data;
            const void* next_client_attach_parameter =
                (next_client_data->data == NULL) ? wild_card_attach_parameter : next_client_data->data;
            if (((memcmp(wild_card_attach_parameter, next_client_attach_parameter, attach_parameter_size) == 0)) ||
                (memcmp(attach_parameter, next_client_attach_parameter, attach_parameter_size) == 0)) {
                EBPF_EXT_LOG_MESSAGE(
                    EBPF_EXT_TRACELOG_LEVEL_ERROR,
                    EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
                    "Attach denied as other clients present with wildcard/exact attach parameter.");
                result = EBPF_ACCESS_DENIED;
                goto Exit;
            }

            link = link->Flink;
        }
    }

Exit:
    if (lock_held) {
        ExReleaseSpinLockShared(&provider_context->lock, oldIrql);
    }

    EBPF_EXT_RETURN_RESULT(result);
}

void
_ebpf_extension_hook_client_cleanup(_In_opt_ _Frees_ptr_opt_ ebpf_extension_hook_client_t* hook_client)
{
    if (hook_client != NULL) {
        if (hook_client->detach_work_item != NULL) {
            IoFreeWorkItem(hook_client->detach_work_item);
        }
        ExFreePool(hook_client);
    }
}

/**
 * @brief Callback invoked when an eBPF hook NPI client (a.k.a eBPF link object) attaches.
 *
 * @param[in] nmr_binding_handle NMR binding between the client module and the provider module.
 * @param[in] provider_context Provider module's context.
 * @param[in] client_registration_instance Client module's registration data.
 * @param[in] client_binding_context Client module's context for binding with provider.
 * @param[in] client_dispatch Client module's dispatch table. Contains the function pointer
 * to invoke the eBPF program.
 * @param[out] provider_binding_context Pointer to provider module's binding context with the client module.
 * @param[out] provider_dispatch Pointer to provider module's dispatch table.
 * @retval STATUS_SUCCESS The operation succeeded.
 * @retval STATUS_NO_MEMORY Failed to allocate provider binding context.
 * @retval STATUS_INVALID_PARAMETER One or more arguments are incorrect.
 */
static NTSTATUS
_ebpf_extension_hook_provider_attach_client(
    _In_ HANDLE nmr_binding_handle,
    _In_ const void* provider_context,
    _In_ const NPI_REGISTRATION_INSTANCE* client_registration_instance,
    _In_ const void* client_binding_context,
    _In_ const void* client_dispatch,
    _Outptr_ void** provider_binding_context,
    _Outptr_result_maybenull_ const void** provider_dispatch)
{
    NTSTATUS status = STATUS_SUCCESS;
    ebpf_extension_hook_provider_t* local_provider_context = (ebpf_extension_hook_provider_t*)provider_context;
    ebpf_extension_hook_client_t* hook_client = NULL;
    ebpf_extension_program_dispatch_table_t* client_dispatch_table;
    ebpf_result_t result = EBPF_SUCCESS;

    EBPF_EXT_LOG_ENTRY();

    if ((provider_binding_context == NULL) || (provider_dispatch == NULL) || (local_provider_context == NULL)) {
        EBPF_EXT_LOG_MESSAGE(
            EBPF_EXT_TRACELOG_LEVEL_ERROR,
            EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
            "Unexpected NULL argument(s). Attach attempt rejected.");
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    *provider_binding_context = NULL;
    *provider_dispatch = NULL;

    hook_client = (ebpf_extension_hook_client_t*)ExAllocatePoolUninitialized(
        NonPagedPoolNx, sizeof(ebpf_extension_hook_client_t), EBPF_EXTENSION_POOL_TAG);
    EBPF_EXT_BAIL_ON_ALLOC_FAILURE_STATUS(EBPF_EXT_TRACELOG_KEYWORD_EXTENSION, hook_client, "hook_client", status);

    memset(hook_client, 0, sizeof(ebpf_extension_hook_client_t));

    hook_client->detach_work_item = NULL;
    hook_client->nmr_binding_handle = nmr_binding_handle;
    hook_client->client_module_id = client_registration_instance->ModuleId->Guid;
    hook_client->client_binding_context = client_binding_context;
    hook_client->client_data = (const ebpf_extension_data_t*)client_registration_instance->NpiSpecificCharacteristics;
    client_dispatch_table = (ebpf_extension_program_dispatch_table_t*)client_dispatch;
    if (client_dispatch_table == NULL) {
        status = STATUS_INVALID_PARAMETER;
        EBPF_EXT_LOG_MESSAGE(
            EBPF_EXT_TRACELOG_LEVEL_ERROR,
            EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
            "client_dispatch_table is NULL. Attach attempt rejected.");
        goto Exit;
    }
    hook_client->invoke_program = client_dispatch_table->ebpf_program_invoke_function;
    hook_client->provider_context = local_provider_context;

    status = _ebpf_ext_attach_init_rundown(hook_client);
    if (!NT_SUCCESS(status)) {
        EBPF_EXT_LOG_MESSAGE_NTSTATUS(
            EBPF_EXT_TRACELOG_LEVEL_ERROR,
            EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
            "_ebpf_ext_attach_init_rundown failed. Attach attempt rejected.",
            status);
        goto Exit;
    }

    // Invoke the hook specific callback to process client attach.
    result = local_provider_context->attach_callback(hook_client, local_provider_context);

    if (result == EBPF_SUCCESS) {
        KIRQL oldIrql = ExAcquireSpinLockExclusive(&local_provider_context->lock);
        InsertTailList(&local_provider_context->attached_clients_list, &hook_client->link);
        ExReleaseSpinLockExclusive(&local_provider_context->lock, oldIrql);
    } else {
        EBPF_EXT_LOG_MESSAGE_UINT32(
            EBPF_EXT_TRACELOG_LEVEL_ERROR,
            EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
            "attach_callback returned failure. Attach attempt rejected.",
            result);
        status = STATUS_ACCESS_DENIED;
    }

Exit:
    if (NT_SUCCESS(status)) {
        *provider_binding_context = hook_client;
        hook_client = NULL;
    } else {
        _ebpf_extension_hook_client_cleanup(hook_client);
    }

    EBPF_EXT_RETURN_NTSTATUS(status);
}

/**
 * @brief Callback invoked when a hook NPI client (a.k.a. eBPF link object) detaches.
 *
 * @param[in] provider_binding_context Provider module's context for binding with the client.
 * @retval STATUS_SUCCESS The operation succeeded.
 * @retval STATUS_PENDING The operation is pending completion.
 * @retval STATUS_INVALID_PARAMETER One or more parameters are invalid.
 */
static NTSTATUS
_ebpf_extension_hook_provider_detach_client(_In_ const void* provider_binding_context)
{
    NTSTATUS status = STATUS_PENDING;
    KIRQL oldIrql;

    EBPF_EXT_LOG_ENTRY();

    ebpf_extension_hook_client_t* local_client_context = (ebpf_extension_hook_client_t*)provider_binding_context;

    ebpf_extension_hook_provider_t* local_provider_context = local_client_context->provider_context;

    if (local_client_context == NULL) {
        EBPF_EXT_LOG_MESSAGE(
            EBPF_EXT_TRACELOG_LEVEL_ERROR,
            EBPF_EXT_TRACELOG_KEYWORD_EXTENSION,
            "local_client_context is NULL. Detach attempt rejected.");
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    // Invoke hook specific handler for processing client detach.
    local_provider_context->detach_callback(local_client_context);

    oldIrql = ExAcquireSpinLockExclusive(&local_provider_context->lock);
    RemoveEntryList(&local_client_context->link);
    ExReleaseSpinLockExclusive(&local_provider_context->lock, oldIrql);

    IoQueueWorkItem(
        local_client_context->detach_work_item,
        _ebpf_extension_detach_client_completion,
        DelayedWorkQueue,
        (void*)local_client_context);

Exit:
    EBPF_EXT_RETURN_NTSTATUS(status);
}

static void
_ebpf_extension_hook_provider_cleanup_binding_context(_Frees_ptr_ void* provider_binding_context)
{
    if (provider_binding_context != NULL) {
        ExFreePool(provider_binding_context);
    }
}

void
ebpf_extension_hook_provider_unregister(_In_opt_ _Frees_ptr_opt_ ebpf_extension_hook_provider_t* provider_context)
{
    EBPF_EXT_LOG_ENTRY();
    if (provider_context != NULL) {
        if (provider_context->nmr_provider_handle != NULL) {
            NTSTATUS status = NmrDeregisterProvider(provider_context->nmr_provider_handle);
            if (status == STATUS_PENDING) {

                // Wait for clients to detach.
                NmrWaitForProviderDeregisterComplete(provider_context->nmr_provider_handle);
            } else {
                EBPF_EXT_LOG_NTSTATUS_API_FAILURE(EBPF_EXT_TRACELOG_KEYWORD_EXTENSION, "NmrDeregisterProvider", status);
            }
        }
        ExFreePool(provider_context);
    }
    EBPF_EXT_LOG_EXIT();
}

NTSTATUS
ebpf_extension_hook_provider_register(
    _In_ const ebpf_extension_hook_provider_parameters_t* parameters,
    _In_ ebpf_extension_hook_on_client_attach attach_callback,
    _In_ ebpf_extension_hook_on_client_detach detach_callback,
    _In_opt_ const void* custom_data,
    _Outptr_ ebpf_extension_hook_provider_t** provider_context)
{
    NTSTATUS status = STATUS_SUCCESS;
    ebpf_extension_hook_provider_t* local_provider_context = NULL;
    NPI_PROVIDER_CHARACTERISTICS* characteristics;

    EBPF_EXT_LOG_ENTRY();
    local_provider_context = (ebpf_extension_hook_provider_t*)ExAllocatePoolUninitialized(
        NonPagedPoolNx, sizeof(ebpf_extension_hook_provider_t), EBPF_EXTENSION_POOL_TAG);
    EBPF_EXT_BAIL_ON_ALLOC_FAILURE_STATUS(
        EBPF_EXT_TRACELOG_KEYWORD_EXTENSION, local_provider_context, "local_provider_context", status);

    memset(local_provider_context, 0, sizeof(ebpf_extension_hook_provider_t));
    InitializeListHead(&local_provider_context->attached_clients_list);

    characteristics = &local_provider_context->characteristics;
    characteristics->Length = sizeof(NPI_PROVIDER_CHARACTERISTICS);
    characteristics->ProviderAttachClient = (PNPI_PROVIDER_ATTACH_CLIENT_FN)_ebpf_extension_hook_provider_attach_client;
    characteristics->ProviderDetachClient = (PNPI_PROVIDER_DETACH_CLIENT_FN)_ebpf_extension_hook_provider_detach_client;
    characteristics->ProviderCleanupBindingContext = _ebpf_extension_hook_provider_cleanup_binding_context;
    characteristics->ProviderRegistrationInstance.Size = sizeof(NPI_REGISTRATION_INSTANCE);
    characteristics->ProviderRegistrationInstance.NpiId = &EBPF_HOOK_EXTENSION_IID;
    characteristics->ProviderRegistrationInstance.NpiSpecificCharacteristics = parameters->provider_data;
    characteristics->ProviderRegistrationInstance.ModuleId = parameters->provider_module_id;

    local_provider_context->attach_callback = attach_callback;
    local_provider_context->detach_callback = detach_callback;
    local_provider_context->custom_data = custom_data;

    status = NmrRegisterProvider(characteristics, local_provider_context, &local_provider_context->nmr_provider_handle);
    if (!NT_SUCCESS(status)) {

        // The docs don't mention the (out) handle status on failure, so explicitly mark it as invalid.
        local_provider_context->nmr_provider_handle = NULL;
        EBPF_EXT_LOG_NTSTATUS_API_FAILURE(EBPF_EXT_TRACELOG_KEYWORD_EXTENSION, "NmrRegisterProvider", status);
        goto Exit;
    }

    *provider_context = local_provider_context;
    local_provider_context = NULL;

Exit:
    if (!NT_SUCCESS(status)) {
        ebpf_extension_hook_provider_unregister(local_provider_context);
    }

    EBPF_EXT_RETURN_NTSTATUS(status);
}

ebpf_extension_hook_client_t*
ebpf_extension_hook_get_attached_client(_Inout_ ebpf_extension_hook_provider_t* provider_context)
{
    ebpf_extension_hook_client_t* client_context = NULL;
    KIRQL oldIrql = ExAcquireSpinLockShared(&provider_context->lock);
    if (!IsListEmpty(&provider_context->attached_clients_list)) {
        client_context = (ebpf_extension_hook_client_t*)CONTAINING_RECORD(
            provider_context->attached_clients_list.Flink, ebpf_extension_hook_client_t, link);
    }
    ExReleaseSpinLockShared(&provider_context->lock, oldIrql);
    return client_context;
}

ebpf_extension_hook_client_t*
ebpf_extension_hook_get_next_attached_client(
    _Inout_ ebpf_extension_hook_provider_t* provider_context,
    _In_opt_ const ebpf_extension_hook_client_t* client_context)
{
    ebpf_extension_hook_client_t* next_client = NULL;
    KIRQL oldIrql = ExAcquireSpinLockShared(&provider_context->lock);
    if (client_context == NULL) {
        // Return the first attached client (if any).
        if (!IsListEmpty(&provider_context->attached_clients_list)) {
            next_client = (ebpf_extension_hook_client_t*)CONTAINING_RECORD(
                provider_context->attached_clients_list.Flink, ebpf_extension_hook_client_t, link);
        }
    } else {
        // Return the next client, unless this is the last one.
        if (client_context->link.Flink != &provider_context->attached_clients_list) {
            next_client = (ebpf_extension_hook_client_t*)CONTAINING_RECORD(
                client_context->link.Flink, ebpf_extension_hook_client_t, link);
        }
    }
    ExReleaseSpinLockShared(&provider_context->lock, oldIrql);
    return next_client;
}
