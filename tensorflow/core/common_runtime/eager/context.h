/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_H_
#define TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_H_

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

// clang-format off
// Required for IS_MOBILE_PLATFORM
#include "tensorflow/core/platform/platform.h"
// clang-format on

#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/eager/eager_executor.h"
#include "tensorflow/core/common_runtime/eager/kernel_and_device.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/rendezvous_mgr.h"
#include "tensorflow/core/example/example.pb.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/platform/env.h"
#if !defined(IS_MOBILE_PLATFORM)
#include "tensorflow/core/distributed_runtime/eager/eager_client.h"
#include "tensorflow/core/distributed_runtime/rendezvous_mgr_interface.h"
#include "tensorflow/core/distributed_runtime/server_lib.h"
#include "tensorflow/core/distributed_runtime/worker_cache.h"
#include "tensorflow/core/distributed_runtime/worker_env.h"
#endif  // !IS_MOBILE_PLATFORM
#include "tensorflow/core/framework/collective.h"
#include "tensorflow/core/framework/log_memory.h"
#include "tensorflow/core/framework/rendezvous.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/flatset.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/gtl/stl_util.h"
#include "tensorflow/core/platform/fingerprint.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/public/version.h"

namespace tensorflow {

// Note: there's a copy enum in eager/c_api.h. It should be kept in sync.
enum ContextDevicePlacementPolicy {
  // Running operations with input tensors on the wrong device will fail.
  DEVICE_PLACEMENT_EXPLICIT = 0,
  // Copy the tensor to the right device but log a warning.
  DEVICE_PLACEMENT_WARN = 1,
  // Silently copy the tensor, which has a performance cost since the operation
  // will be blocked till the copy completes. This is the default policy.
  DEVICE_PLACEMENT_SILENT = 2,
  // Placement policy which silently copies int32 tensors but not other dtypes.
  DEVICE_PLACEMENT_SILENT_FOR_INT32 = 3,
};

class RunMetadataListener {
 public:
  virtual ~RunMetadataListener() {}
  virtual void BeforeClearRunMetadata() = 0;
};

class EagerContext : public core::RefCounted {
 public:
  // TODO: remove this constructor once we migrate all callers to the next one.
  EagerContext(const SessionOptions& opts,
               ContextDevicePlacementPolicy default_policy, bool async,
               std::unique_ptr<const DeviceMgr> device_mgr,
               Rendezvous* rendezvous);

  EagerContext(
      const SessionOptions& opts, ContextDevicePlacementPolicy default_policy,
      bool async, const DeviceMgr* device_mgr, bool device_mgr_owned,
      Rendezvous* rendezvous, const CustomKernelCreator* custom_kernel_creator,
      DistributedFunctionLibraryRuntime* cluster_flr = nullptr,
      std::function<Rendezvous*(const int64)> rendezvous_creator = nullptr,
      const DeviceMgr* remote_device_mgr = nullptr);

  ~EagerContext();

  // Returns the function library runtime for the given device.
  FunctionLibraryRuntime* func_lib(const Device* d) const {
    return pflr_->GetFLR(d->name());
  }

  ProcessFunctionLibraryRuntime* pflr() const { return pflr_.get(); }

  // True if running in asynchronous mode.
  bool Async() const;

  EagerExecutor* Executor() { return &executor_; }

  std::function<void(std::function<void()>)>* runner() { return &runner_; }

  // Sets whether this thread should run in synchronous or asynchronous mode.
  Status SetAsyncForThread(bool async);

  // TODO(apassos) make this return a constant reference
  gtl::FlatMap<string, Device*, StringPieceHasher>* device_map() {
    return &devices_map_;
  }

  // TODO(apassos) make this return a constant reference
  std::vector<Device*>* devices() { return &devices_; }
  const std::vector<DeviceType>& prioritized_device_type_list() {
    return prioritized_device_type_list_;
  }

  // Clears the kernel caches.
  void ClearCaches();

  // Sets the device placement policy for the current thread.
  void SetThreadLocalDevicePlacementPolicy(ContextDevicePlacementPolicy policy);

  // Returns the device placement policy for the current thread.
  ContextDevicePlacementPolicy GetDevicePlacementPolicy();

  Status AsyncWait() { return executor_.WaitForAllPendingNodes(); }

  Status GetStatus() { return executor_.status(); }

  void ClearAsyncError() { executor_.ClearError(); }

  bool FindFunctionByName(const string& name);

  Status FindFunctionOpData(const string& name,
                            const tensorflow::OpRegistrationData** op_data);

  const FunctionDef* FindFunctionDef(const string& name);

  Status FindDeviceByName(const string& name, Device** result) const;

  Device* HostCPU() const { return devices_[0]; }

  GraphCollector* GetGraphCollector() { return &graph_collector_; }

  uint64 NextId() { return executor_.NextId(); }

  void ExecutorAdd(EagerNode* node) { executor_.Add(node); }

  Status AddFunctionDef(const FunctionDef& fdef);

  Status RemoveFunction(const string& func);

  core::RefCountPtr<KernelAndDevice> GetCachedKernel(Fprint128 cache_key);

  void AddKernelToCache(Fprint128 cache_key, KernelAndDevice* kernel);

  bool LogDevicePlacement() const { return log_device_placement_; }
  bool LogMemory() const { return log_memory_; }

  Rendezvous* GetRendezvous() const { return rendezvous_; }
  Rendezvous* CreateRendezvous(const int64 step_id) const {
    if (rendezvous_creator_ != nullptr) {
      return rendezvous_creator_(step_id);
    }

#if !defined(IS_MOBILE_PLATFORM)
    if (worker_env_ != nullptr && worker_env_->rendezvous_mgr != nullptr) {
      auto* remote_r = worker_env_->rendezvous_mgr->Find(step_id);
      remote_r->Initialize(worker_session_.get()).IgnoreError();
      return remote_r;
    }
#endif

    if (remote_device_mgr() == nullptr) {
      return new IntraProcessRendezvous(local_device_mgr());
    }

    return nullptr;
  }

  CollectiveExecutorMgrInterface* collective_executor_mgr() {
    return (collective_executor_mgr_ != nullptr)
               ? collective_executor_mgr_.get()
               : unowned_collective_executor_mgr_;
  }
  std::unique_ptr<CollectiveExecutor::Handle> GetCollectiveExecutorHandle() {
    return std::unique_ptr<CollectiveExecutor::Handle>(
        new CollectiveExecutor::Handle(
            collective_executor_mgr()->FindOrCreate(0), true /*inherit_ref*/));
  }

  const tensorflow::DeviceMgr* local_device_mgr() const {
    return (local_device_manager_ != nullptr) ? local_device_manager_.get()
                                              : local_unowned_device_manager_;
  }
  const tensorflow::DeviceMgr* remote_device_mgr() const {
    return (remote_device_manager_ != nullptr) ? remote_device_manager_.get()
                                               : remote_unowned_device_manager_;
  }

  // TODO(apassos) remove the need for this
  void ReleaseDeviceMgr() { local_device_manager_.release(); }

  // TODO(apassos) clean up RunMetadata storage.
  mutex* MetadataMu() LOCK_RETURNED(metadata_mu_) { return &metadata_mu_; }
  bool ShouldStoreStepStats() LOCKS_EXCLUDED(metadata_mu_);
  void SetShouldStoreStepStats(bool value);
  bool ShouldStoreGraphs() LOCKS_EXCLUDED(metadata_mu_);
  void SetShouldStoreGraphs(bool value);
  RunMetadata* RunMetadataProto() { return &run_metadata_; }
  void ClearRunMetadata() EXCLUSIVE_LOCKS_REQUIRED(metadata_mu_);

  Status RegisterRunMetadataListener(RunMetadataListener* listener)
      LOCKS_EXCLUDED(metadata_mu_);
  void ClearRunMetadataListener() LOCKS_EXCLUDED(metadata_mu_);

  void StartStep();
  void EndStep();
  ScopedStepContainer* StepContainer();

  FunctionLibraryDefinition* FuncLibDef() { return &func_lib_def_; }

#if !defined(IS_MOBILE_PLATFORM)
  Status GetClientAndContextID(Device* device, eager::EagerClient** client,
                               uint64* context_id);

  // TODO(nareshmodi): Encapsulate remote state into a separate
  // class/struct.
  //
  // Enables the eager context to communicate with remote devices.
  //
  // - server: A ServerInterface that exports the tensorflow.WorkerService.
  // Note that this class expects the server to already have been started.
  // - remote_eager_workers: A cache from which we can get "EagerClient"s to
  // communicate with remote eager services.
  // - remote_device_mgr: A DeviceMgr* which contains all remote devices
  // (should contain no local devices).
  // - remote_contexts: A map containing task name to remote context ID.
  Status InitializeRemote(
      std::unique_ptr<ServerInterface> server, WorkerEnv* worker_env,
      std::shared_ptr<WorkerSession> worker_session,
      std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
      std::unique_ptr<DeviceMgr> remote_device_manager,
      const gtl::FlatMap<string, uint64>& remote_contexts, Rendezvous* r,
      DeviceMgr* local_device_mgr, int keep_alive_secs,
      DistributedFunctionLibraryRuntime* cluster_flr);

  bool HasActiveRemoteContext(uint64 context_id) {
    return active_remote_contexts_.find(context_id) !=
           active_remote_contexts_.end();
  }

  Status StoreCollectiveOpsServer(
      std::unique_ptr<ServerInterface> server, DeviceMgr* device_mgr,
      CollectiveExecutorMgrInterface* rpc_collective_executor_mgr);
#endif  // IS_MOBILE_PLATFORM

  // If true, then tensors should be shipped across processes via the
  // EagerService.SendTensor RPC. If false, _Send/_Recv ops should be used
  // instead (which in-turn use WorkerService.RecvTensor RPCs).
  bool UseSendTensorRPC() { return use_send_tensor_rpc_; }
  bool PinSmallOpsToCPU() { return pin_small_ops_to_cpu_; }

  tensorflow::Env* TFEnv() const { return env_; }

  // All child threads will be reset() when destructing EagerContext.
  void AddChildThread(std::unique_ptr<Thread> thread);

  Status FindDeviceFromName(const char* device_name, Device** device) const;

  bool IsLocal(const Device* d) const;
  bool OnSameTask(const Device* first, const Device* second) const;
  // Gets the CPU device on the task of device.
  Status CPUDeviceOnTask(const Device* device, Device** cpu_device) const;

 private:
  void InitDeviceMapAndAsync();
  Status MaybeRegisterFunctionRemotely(const FunctionDef& fdef);

  const ContextDevicePlacementPolicy policy_;

  // Note: we cannot use C++11 thread_local here as there is no concept of a
  // thread-local-object-local variable in C++11.
  mutex policy_map_mu_;
  std::unordered_map<std::thread::id, ContextDevicePlacementPolicy>
      thread_local_policies_ GUARDED_BY(policy_map_mu_);

  // Only one of the below is set.
  std::unique_ptr<const DeviceMgr> local_device_manager_;
  const DeviceMgr* local_unowned_device_manager_;

  // Only one of the below is set. remote_unowned_device_manager_ is set on
  // remote worker to allow running multi-device function on remote worker.
  std::unique_ptr<DeviceMgr> remote_device_manager_;
  const DeviceMgr* remote_unowned_device_manager_;

  // Devices owned by device_manager
  std::vector<Device*> devices_;
  std::vector<DeviceType> prioritized_device_type_list_;
  // All devices are not owned.
  gtl::FlatMap<string, Device*, StringPieceHasher> devices_map_;
  Rendezvous* rendezvous_;
  std::function<Rendezvous*(const int64)> rendezvous_creator_;

  mutex functions_mu_;
  FunctionLibraryDefinition func_lib_def_ GUARDED_BY(functions_mu_){
      OpRegistry::Global(), {}};

  std::unique_ptr<thread::ThreadPool> thread_pool_;

  // One FunctionLibraryRuntime per device.
  // func_libs[i] is the FunctionLibraryRuntime corresponding to
  // session->devices[i].
  std::unique_ptr<ProcessFunctionLibraryRuntime> pflr_;

  std::function<void(std::function<void()>)> runner_;

  mutex cache_mu_;
  struct RegisteredFunction : public core::RefCounted {
    ~RegisteredFunction() override {}

    std::unique_ptr<std::vector<Fprint128>> cached_kernel_keys;
  };
  std::unordered_map<Fprint128, core::RefCountPtr<KernelAndDevice>,
                     Fprint128Hasher>
      kernel_cache_ GUARDED_BY(cache_mu_);
  std::unordered_map<string, RegisteredFunction*> registered_functions_
      GUARDED_BY(cache_mu_);

  // Whether we should compute RunMetadata.
  std::atomic<bool> should_store_step_stats_{false};
  std::atomic<bool> should_store_graphs_{false};
  mutex metadata_mu_;
  RunMetadata run_metadata_ GUARDED_BY(metadata_mu_);
  RunMetadataListener* metadata_listener_ GUARDED_BY(metadata_mu_) = nullptr;
  GraphCollector graph_collector_;
  const bool log_device_placement_;
  // EagerExecutor for async execution.
  EagerExecutor executor_;

  // Information related to step containers.
  std::atomic<int> num_active_steps_;
  std::unique_ptr<ScopedStepContainer> step_container_ GUARDED_BY(metadata_mu_);

  // True if the default value for execution mode is async. Note that this value
  // can be overridden per thread based on `thread_local_async` overrides.
  const bool async_default_;
  mutable mutex async_map_mu_;
  std::unordered_map<std::thread::id, bool> thread_local_async_
      GUARDED_BY(async_map_mu_);

  const bool log_memory_;

  Env* const env_;

  std::unique_ptr<CollectiveExecutorMgrInterface> collective_executor_mgr_;
  CollectiveExecutorMgrInterface* unowned_collective_executor_mgr_ = nullptr;

#if !defined(IS_MOBILE_PLATFORM)
  void CloseRemoteContexts();

  // The server_ is not const since we release it when the context is destroyed.
  // Therefore the server_ object is not marked as const (even though it should
  // be).
  std::unique_ptr<ServerInterface> server_;
  WorkerEnv* worker_env_ = nullptr;
  std::shared_ptr<WorkerSession> worker_session_;
  std::unique_ptr<eager::EagerClientCache> remote_eager_workers_;

  mutex remote_state_mu_;

  gtl::FlatMap<string, uint64> remote_contexts_;
  gtl::FlatSet<uint64> active_remote_contexts_;
  gtl::FlatMap<Device*, std::pair<eager::EagerClient*, uint64>>
      device_to_client_cache_;

  int keep_alive_secs_ GUARDED_BY(remote_state_mu_);
  std::atomic<int> sleep_for_secs_;

  std::unique_ptr<Thread> keep_alive_thread_;
  mutex keep_alive_thread_shutdown_mu_;
  condition_variable keep_alive_thread_cv_;
  bool shutting_down_ GUARDED_BY(keep_alive_thread_shutdown_mu_) = false;
#endif  // IS_MOBILE_PLATFORM

  bool use_send_tensor_rpc_;
  const bool pin_small_ops_to_cpu_;
  std::vector<std::unique_ptr<tensorflow::Thread>> child_threads_;
};

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_COMMON_RUNTIME_EAGER_CONTEXT_H_
