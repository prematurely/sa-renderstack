# API7 Selective State Journal (C++23)

独立 C++23 SDK consumer：`include/`+`src/` 提供 `SelectiveJournal` RAII（same-thread、begin/capture/restore、Reset 失效和 fail-closed），`main.cpp` 是动态加载 backend 的 contract probe。

`SelectiveJournal` 不可复制，可以移动；移动会转移 COM 引用和事务，但不会改变
所属线程。同线程析构或移动赋值会先恢复仍在进行的事务，再释放接口。活动对象必须
在所属线程恢复、移动和销毁；跨线程的 begin/capture/restore 会拒绝调用后端。
Reset 后的 restore 失败会关闭本地事务标记，之后可以重新 begin。

```powershell
cmake -S . -B build; cmake --build build --config Release
build\\Release\\api7_selective_journal.exe path\\to\\d3d9.dll
```

项目遵循 C++23 RAII 和 contract testing，使用公开 `backend_api.h` 与 Microsoft D3D9 `Reset/SetRenderState` 合约。`same_thread_sequence` 指该 probe 在同一线程执行完整事务；生产实现不应把 probe 的线程假设扩大成未声明的 ABI 保证。该项目只验证 API7，不伪造 ProperShaders 生产 consumer 或性能收益。
