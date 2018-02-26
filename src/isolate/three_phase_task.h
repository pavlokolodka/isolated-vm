#pragma once
#include "node-wrapper.h"
#include "environment.h"
#include "holder.h"
#include "remote_handle.h"
#include "util.h"
#include <memory>

namespace ivm {

/**
 * Most operations in this library can be decomposed into three phases.
 *
 * - Phase 1 [Isolate 1]: copy data out of current isolate
 * - Phase 2 [Isolate 2]: copy data into new isolate, run work, copy data out of isolate
 * - Phase 3 [Isolate 1]: copy results from phase 2 into the original isolate
 *
 * This class handles the locking and thread synchronization for either synchronous or
 * asynchronous functions. That way the same code can be used for both versions of each function.
 *
 * When `async=true` a promise is return which will be resolved after all the work is done
 * When `async=false` this will run in the calling thread until completion
 */
class ThreePhaseTask {
	private:
		/**
		 * Contains references back to the original isolate which will be used after phase 2 to wake the
		 * isolate up and begin phase 3
		 */
		struct CalleeInfo {
			RemoteTuple<v8::Promise::Resolver, v8::Context, v8::StackTrace> remotes;
			node::async_context async;
			CalleeInfo(
				v8::Local<v8::Promise::Resolver> resolver,
				v8::Local<v8::Context> context,
				v8::Local<v8::StackTrace> stack_trace
			);
			~CalleeInfo();
		};

		/**
		 * Class which manages running async phase 2, then phase 3
		 */
		struct Phase2Runner : public Runnable {
			std::unique_ptr<ThreePhaseTask> self;
			std::unique_ptr<CalleeInfo> info;
			bool did_run = false;

			Phase2Runner(
				std::unique_ptr<ThreePhaseTask> self,
				std::unique_ptr<CalleeInfo> info
			);
			Phase2Runner(const Phase2Runner&) = delete;
			Phase2Runner& operator= (const Phase2Runner&) = delete;
			~Phase2Runner() final;
			void Run() final;
		};

		/**
		 * Class which manages running async phase 2 in ignored mode (ie no phase 3)
		 */
		struct Phase2RunnerIgnored : public Runnable {
			std::unique_ptr<ThreePhaseTask> self;
			explicit Phase2RunnerIgnored(std::unique_ptr<ThreePhaseTask> self);
			void Run() final;
		};

		v8::Local<v8::Value> RunSync(IsolateHolder& second_isolate);

	public:
		ThreePhaseTask() = default;
		ThreePhaseTask(const ThreePhaseTask&) = delete;
		ThreePhaseTask& operator= (const ThreePhaseTask&) = delete;
		virtual ~ThreePhaseTask() = default;

		virtual void Phase2() = 0;
		virtual v8::Local<v8::Value> Phase3() = 0;

		template <int async, typename T, typename ...Args>
		static v8::Local<v8::Value> Run(IsolateHolder& second_isolate, Args&&... args) {

			if (async == 1) { // Full async, promise returned
				// Build a promise for outer isolate
				v8::Isolate* isolate = v8::Isolate::GetCurrent();
				auto context_local = isolate->GetCurrentContext();
				auto promise_local = Unmaybe(v8::Promise::Resolver::New(context_local));
				v8::TryCatch try_catch(isolate);
				try {
					// Schedule Phase2 async
					second_isolate.ScheduleTask(
						std::make_unique<Phase2Runner>(
							std::make_unique<T>(std::forward<Args>(args)...), // <-- Phase1 / ctor called here
							std::make_unique<CalleeInfo>(
								promise_local,
								context_local,
								v8::StackTrace::CurrentStackTrace(isolate, 10)
							)
						), false, true
					);
				} catch (const js_runtime_error& cc_error) {
					// An error was caught while running ctor (phase 1)
					assert(try_catch.HasCaught());
					v8::Maybe<bool> ret = promise_local->Reject(context_local, try_catch.Exception());
					try_catch.Reset();
					Unmaybe(ret);
				}
				return promise_local->GetPromise();
			} else if (async == 2) { // Async, promise ignored
				// Schedule Phase2 async
				second_isolate.ScheduleTask(
					std::make_unique<Phase2RunnerIgnored>(
						std::make_unique<T>(std::forward<Args>(args)...) // <-- Phase1 / ctor called here
					), false, true
				);
				return v8::Undefined(v8::Isolate::GetCurrent());
			} else {
				// Execute syncronously
				T self(std::forward<Args>(args)...);
				return self.RunSync(second_isolate);
			}
		}
};

} // namespace ivm
