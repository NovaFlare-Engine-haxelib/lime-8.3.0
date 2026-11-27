#include "SDLApplication.h"
#include "SDLWindow.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include <graphics/RenderThread.h>
#include <graphics/FrameTimeline.h>
#include <graphics/AndroidPerformanceHints.h>
#include "../../graphics/opengl/OpenGLBindings.h"
#include <system/System.h>
#include <map>
#include <stdint.h>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef HX_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define LIME_PAUSE() _mm_pause()
#elif defined(__arm__) || defined(__aarch64__) || defined(_M_ARM) || defined(_M_ARM64)
#define LIME_PAUSE() __asm__ __volatile__ ("yield")
#else
#define LIME_PAUSE()
#endif

#ifdef EMSCRIPTEN
#include "emscripten.h"
#endif


namespace lime {

	static void BeginTimelinePhase() {
		if (!FrameTimeline::CurrentWork() || !OpenGLBindings::isMultiThreaded) return;
		if (auto* thread = OpenGLBindings::renderThread) thread->BeginTimelinePhase();
	}

	static void DispatchScheduledRender(RenderEvent* event, double periodMs) {
		const bool threaded = OpenGLBindings::isMultiThreaded && OpenGLBindings::renderThread
			&& OpenGLBindings::renderThread->IsRunning();
		AndroidPerformanceHints::BeginRender(periodMs, threaded);
		FrameTimeline::BeginRender();
		BeginTimelinePhase();
		if (!OpenGLBindings::isMultiThreaded)
			FrameTimeline::BeginGpu(FrameTimeline::CurrentRender());
		RenderEvent::Dispatch(event);
		FrameTimeline::EndRender();
		AndroidPerformanceHints::EndRender();
	}

	static void TraceFrameSetting(const char* name, std::uint64_t value) {
		const auto now = FrameTimeline::Now();
		if (now) FrameTimeline::Write({name, 0, now, now, value});
	}


	AutoGCRoot* Application::callback = 0;
	SDLApplication* SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	static std::map<int, std::map<int, int>> gamepadsAxisMap;
	bool inBackground = false;

	#ifdef _WIN32
	static double FrameRateOverride (const char* name, double fallback) {
		const char* value = std::getenv (name);
		if (value == NULL || *value == '\0') return fallback;
		char* end = NULL;
		const double parsed = std::strtod (value, &end);
		return end != value && *end == '\0' && parsed > 0.0
			? parsed : fallback;
	}
	#endif

	static Uint64 FramePeriodTicks (double periodMs, Uint64 frequency) {
		const auto ticks = static_cast<Uint64>(periodMs * frequency / 1000.0);
		return ticks ? ticks : 1;
	}

	static Uint64 AdvanceFrameDeadline (Uint64 deadline, Uint64 period, Uint64 now) {
#ifdef ANDROID
		return now + (period - (now - deadline) % period);
#else
		const auto next = deadline + period;
		return next > now ? next : now + period;
#endif
	}

	#if !defined(IPHONE) && !defined(EMSCRIPTEN)
	// Sleep against an absolute high-resolution deadline and reserve only the
	// final 100 microseconds for a short spin. The previous high-FPS path used
	// _mm_pause for the whole 0.5-1.0 ms interval, permanently consuming a CPU
	// core and starving the render/GC workers that were supposed to help it.
	static void WaitForFrameDeadline (uint64_t deadline, uint64_t frequency) {
		if (frequency == 0) return;
		uint64_t now = SDL_GetPerformanceCounter ();
		if (now >= deadline) return;

		const uint64_t spinReserve = frequency / 10000; // 100 us
		const uint64_t remaining = deadline - now;
		if (remaining > spinReserve) {
			const uint64_t waitTicks = remaining - spinReserve;
			#ifdef _WIN32
			struct HighResolutionTimer {
				HANDLE handle;
				HighResolutionTimer () : handle (CreateWaitableTimerExW (
					NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
					TIMER_MODIFY_STATE | SYNCHRONIZE)) {
					if (handle == NULL)
						handle = CreateWaitableTimerW (NULL, FALSE, NULL);
				}
				~HighResolutionTimer () {
					if (handle != NULL) CloseHandle (handle);
				}
			};
			static thread_local HighResolutionTimer timer;
			if (timer.handle != NULL) {
				const uint64_t wait100ns = waitTicks * 10000000ULL / frequency;
				if (wait100ns > 0) {
					LARGE_INTEGER dueTime;
					dueTime.QuadPart = -static_cast<LONGLONG> (wait100ns);
					if (SetWaitableTimer (timer.handle, &dueTime, 0, NULL,
						NULL, FALSE))
						WaitForSingleObject (timer.handle, INFINITE);
				}
			} else {
				std::this_thread::yield ();
			}
			#else
			const uint64_t waitMicros = waitTicks * 1000000ULL / frequency;
			if (waitMicros > 0)
				std::this_thread::sleep_for (
					std::chrono::microseconds (waitMicros));
			#endif
		}

		while ((now = SDL_GetPerformanceCounter ()) < deadline)
			LIME_PAUSE ();
	}


	#endif


	SDLApplication::SDLApplication () {

		initFlags = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER | SDL_INIT_JOYSTICK;
		#if defined(LIME_MOJOAL) || defined(LIME_OPENALSOFT)
		initFlags |= SDL_INIT_AUDIO;
		#endif

		if (SDL_Init (initFlags) != 0) {

			printf ("Could not initialize SDL: %s.\n", SDL_GetError ());

		}

		SDL_LogSetPriority (SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);

		currentApplication = this;
		framePeriod = 50.0;
		renderFramePeriod = 1000.0 / 60.0;
		lockRender = false;


		ApplicationEvent applicationEvent;
		ClipboardEvent clipboardEvent;
		DropEvent dropEvent;
		GamepadEvent gamepadEvent;
		JoystickEvent joystickEvent;
		KeyEvent keyEvent;
		MouseEvent mouseEvent;
		OrientationEvent orientationEvent;
		RenderEvent renderEvent;
		SensorEvent sensorEvent;
		TextEvent textEvent;
		TouchEvent touchEvent;
		WindowEvent windowEvent;

		SDL_EventState (SDL_DROPFILE, SDL_ENABLE);
		SDLJoystick::Init ();

		#if defined(_WIN32) || defined(HX_MACOS)
		SDL_AddEventWatch (HandleEventWatch, this);
		#endif

		#ifdef HX_MACOS
		CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL (CFBundleGetMainBundle ());
		char path[PATH_MAX];

		if (CFURLGetFileSystemRepresentation (resourcesURL, TRUE, (UInt8 *)path, PATH_MAX)) {

			chdir (path);

		}

		CFRelease (resourcesURL);
		#endif

	}


	SDLApplication::~SDLApplication () {

		#if defined(_WIN32) || defined(HX_MACOS)
		SDL_DelEventWatch (HandleEventWatch, this);
		#endif

	}


	int SDLApplication::Exec () {
		
		#ifdef _WIN32
		timeBeginPeriod (1);
		#endif

		Init ();

		#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop ();
		emscripten_set_main_loop (UpdateFrame, 0, 0);
		emscripten_set_main_loop_timing (EM_TIMING_RAF, 1);
		#endif

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		return 0;

		#else

		while (active) {

			Update ();

		}

		return Quit ();

		#endif

	}


	void SDLApplication::DispatchScheduledUpdate () {

		applicationEvent.type = UPDATE;
		applicationEvent.deltaTime = framePeriod;
		AndroidPerformanceHints::BeginUpdate();
		FrameTimeline::BeginUpdate();
		BeginTimelinePhase();
		const auto update = FrameTimeline::CurrentUpdate();
		{
			FrameTimeline::Span duration("update", update);
			ApplicationEvent::Dispatch (&applicationEvent);
		}

		renderEvent.type = RENDER_UPDATE;
		{
			FrameTimeline::Span duration("render_update", update);
			RenderEvent::Dispatch (&renderEvent);
		}

		FrameTimeline::EndUpdate();
		AndroidPerformanceHints::EndUpdate();

		if (!lockRender) TryDispatchRender (framePeriod);

	}


	bool SDLApplication::TryDispatchRender (double periodMs) {

		if (!active || inBackground || !OpenGLBindings::CanRender()) return false;

		auto* thread = OpenGLBindings::renderThread;
		if (OpenGLBindings::isMultiThreaded && thread && thread->IsRunning()) {
			const int pending = RenderThread::activePendingFrames.load (std::memory_order_acquire);
			// Starting a draw while a vsynced swap is pending can strand a
			// synchronous GL request behind the previous presentation.
			#ifdef IPHONE
			// UIKit drives frames with a display link and exposes no swap interval.
			const int pendingLimit = 1;
			#else
			const int pendingLimit = thread->CurrentSwapInterval () != 0
				? 1 : RenderThread::MaxPendingFrames;
			#endif
			if (pending >= pendingLimit) {
				FrameTimeline::RenderBlocked (pending);
				if (lockRender && pendingLimit == 1) {
#ifdef ANDROID
					renderRetryRequested = true;
#else
					windowRedrawRequested = true;
#endif
				}
				return false;
			}
		}

#ifdef ANDROID
		if (renderRetryRequested) {
			renderRetryRequested = false;
			// A late retry consumes a draw slot. Leave a full period before the next scheduled draw.
			nextRenderCounter = SDL_GetPerformanceCounter () + renderPeriodTicks;
		}
#endif
		windowRedrawRequested = false;
		renderEvent.type = RENDER;
		DispatchScheduledRender (&renderEvent, periodMs);
		return true;

	}


	void SDLApplication::DispatchPendingRenderDeviceReset () {

		if (!renderDeviceResetPending || inBackground || !OpenGLBindings::CanRender()) return;

		renderDeviceResetPending = false;
		renderEvent.type = RENDER_CONTEXT_RESTORED;
		RenderEvent::Dispatch (&renderEvent);
		renderEvent.type = RENDER;

	}


	void SDLApplication::HandleEvent (SDL_Event* event) {

		switch (event->type) {

			case SDL_APP_WILLENTERBACKGROUND:

				AndroidPerformanceHints::SetForeground(false);
				inBackground = true;
				SDLWindow::PauseRendering();

				windowEvent.type = WINDOW_DEACTIVATE;
				WindowEvent::Dispatch (&windowEvent);
				break;

			case SDL_APP_WILLENTERFOREGROUND:

				break;

			case SDL_APP_DIDENTERFOREGROUND:
				AndroidPerformanceHints::SetForeground(true);

				SDLWindow::ResumeRendering();
				windowEvent.type = WINDOW_ACTIVATE;
				WindowEvent::Dispatch (&windowEvent);

				inBackground = false;
				ResetFrameDeadlines ();
				break;

			case SDL_CLIPBOARDUPDATE:

				ProcessClipboardEvent (event);
				break;

			case SDL_CONTROLLERAXISMOTION:
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:

				ProcessGamepadEvent (event);
				break;

			case SDL_DISPLAYEVENT:

				switch (event->display.event) {

					case SDL_DISPLAYEVENT_ORIENTATION:

						// this is the orientation of what is rendered, which
						// may not exactly match the orientation of the device,
						// if the app was locked to portrait or landscape.
						orientationEvent.type = DISPLAY_ORIENTATION_CHANGE;
						orientationEvent.orientation = event->display.data1;
						orientationEvent.display = event->display.display;
						OrientationEvent::Dispatch (&orientationEvent);

						break;

				}
				break;

			case SDL_DROPFILE:

				ProcessDropEvent (event);
				break;

			case SDL_FINGERMOTION:
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:

				ProcessTouchEvent (event);
				break;

			case SDL_JOYAXISMOTION:

				if (SDLJoystick::IsAccelerometer (event->jaxis.which)) {

					ProcessSensorEvent (event);

				} else {

					ProcessJoystickEvent (event);

				}

				break;

			case SDL_JOYBALLMOTION:
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
			case SDL_JOYHATMOTION:
			case SDL_JOYDEVICEADDED:
			case SDL_JOYDEVICEREMOVED:

				ProcessJoystickEvent (event);
				break;

			case SDL_KEYDOWN:
			case SDL_KEYUP:

				ProcessKeyEvent (event);
				break;

			case SDL_MOUSEMOTION:
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				ProcessMouseEvent (event);
				break;

			case SDL_MOUSEWHEEL:

				ProcessMouseEvent (event);
				break;

			#ifndef EMSCRIPTEN
			case SDL_RENDER_DEVICE_RESET:

				SDLWindow::OnRenderDeviceReset();
				renderEvent.type = RENDER_CONTEXT_LOST;
				RenderEvent::Dispatch (&renderEvent);
				renderDeviceResetPending = true;
				DispatchPendingRenderDeviceReset ();
				break;
			#endif

            case SDL_TEXTINPUT:
            case SDL_TEXTEDITING:
            case SDL_TEXTEDITING_EXT:

                ProcessTextEvent (event);
                break;

			case SDL_WINDOWEVENT:

				switch (event->window.event) {

					case SDL_WINDOWEVENT_ENTER:
					case SDL_WINDOWEVENT_LEAVE:
					case SDL_WINDOWEVENT_SHOWN:
					case SDL_WINDOWEVENT_HIDDEN:
					case SDL_WINDOWEVENT_FOCUS_GAINED:
					case SDL_WINDOWEVENT_FOCUS_LOST:
					case SDL_WINDOWEVENT_MAXIMIZED:
					case SDL_WINDOWEVENT_MINIMIZED:
					case SDL_WINDOWEVENT_MOVED:
					case SDL_WINDOWEVENT_RESTORED:

						ProcessWindowEvent (event);
						break;

					case SDL_WINDOWEVENT_EXPOSED:
					case SDL_WINDOWEVENT_SIZE_CHANGED:

						ProcessWindowEvent (event);
						if (!inBackground) windowRedrawRequested = true;
						break;

					case SDL_WINDOWEVENT_DISPLAY_CHANGED:
						if (FrameTimeline::Now()) FrameTimeline::WindowGeometry(SDL_GetWindowFromID(event->window.windowID));
						break;

					case SDL_WINDOWEVENT_CLOSE:

						ProcessWindowEvent (event);

						// Avoid handling SDL_QUIT if in response to window.close
						SDL_Event event;

						if (SDL_PollEvent (&event)) {

							if (event.type != SDL_QUIT) {

								HandleEvent (&event);

							}

						}
						break;

				}

				break;

			case SDL_QUIT:

				active = false;
				break;

		}

	}


	void SDLApplication::Init () {

		active = true;
		ResetFrameDeadlines ();

	}


	void SDLApplication::ResetFrameDeadlines () {

		if (!performanceFrequency) performanceFrequency = SDL_GetPerformanceFrequency ();
		const auto now = SDL_GetPerformanceCounter ();
		updatePeriodTicks = FramePeriodTicks (framePeriod, performanceFrequency);
		renderPeriodTicks = FramePeriodTicks (renderFramePeriod, performanceFrequency);
		nextUpdateCounter = now + updatePeriodTicks;
		nextRenderCounter = now + renderPeriodTicks;
#ifdef ANDROID
		renderRetryRequested = false;
#endif

	}


	void SDLApplication::ProcessClipboardEvent (SDL_Event* event) {

		if (ClipboardEvent::callback) {

			clipboardEvent.type = CLIPBOARD_UPDATE;

			ClipboardEvent::Dispatch (&clipboardEvent);

		}

	}


	void SDLApplication::ProcessDropEvent (SDL_Event* event) {

		if (DropEvent::callback) {

			dropEvent.type = DROP_FILE;
			dropEvent.file = (vbyte*)event->drop.file;

			DropEvent::Dispatch (&dropEvent);
			SDL_free (dropEvent.file);

		}

	}


	void SDLApplication::ProcessGamepadEvent (SDL_Event* event) {

		if (GamepadEvent::callback) {

			switch (event->type) {

				case SDL_CONTROLLERAXISMOTION:

					if (gamepadsAxisMap[event->caxis.which].empty ()) {

						gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;

					} else if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] == event->caxis.value) {

						break;

					}

					gamepadEvent.type = GAMEPAD_AXIS_MOVE;
					gamepadEvent.axis = event->caxis.axis;
					gamepadEvent.id = event->caxis.which;

					if (event->caxis.value > -analogAxisDeadZone && event->caxis.value < analogAxisDeadZone) {

						if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] != 0) {

							gamepadsAxisMap[event->caxis.which][event->caxis.axis] = 0;
							gamepadEvent.axisValue = 0;
							GamepadEvent::Dispatch (&gamepadEvent);

						}

						break;

					}

					gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;
					gamepadEvent.axisValue = event->caxis.value / (event->caxis.value > 0 ? 32767.0 : 32768.0);

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERBUTTONDOWN:

					gamepadEvent.type = GAMEPAD_BUTTON_DOWN;
					gamepadEvent.button = event->cbutton.button;
					gamepadEvent.id = event->cbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERBUTTONUP:

					gamepadEvent.type = GAMEPAD_BUTTON_UP;
					gamepadEvent.button = event->cbutton.button;
					gamepadEvent.id = event->cbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERDEVICEADDED:

					if (SDLGamepad::Connect (event->cdevice.which)) {

						gamepadEvent.type = GAMEPAD_CONNECT;
						gamepadEvent.id = SDLGamepad::GetInstanceID (event->cdevice.which);

						GamepadEvent::Dispatch (&gamepadEvent);

					}

					break;

				case SDL_CONTROLLERDEVICEREMOVED: {

					gamepadEvent.type = GAMEPAD_DISCONNECT;
					gamepadEvent.id = event->cdevice.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					SDLGamepad::Disconnect (event->cdevice.which);
					break;

				}

			}

		}

	}


	void SDLApplication::ProcessJoystickEvent (SDL_Event* event) {

		if (JoystickEvent::callback) {

			switch (event->type) {

				case SDL_JOYAXISMOTION:

					if (!SDLJoystick::IsAccelerometer (event->jaxis.which)) {

						joystickEvent.type = JOYSTICK_AXIS_MOVE;
						joystickEvent.index = event->jaxis.axis;
						joystickEvent.x = event->jaxis.value / (event->jaxis.value > 0 ? 32767.0 : 32768.0);
						joystickEvent.id = event->jaxis.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;


				case SDL_JOYBUTTONDOWN:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_DOWN;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYBUTTONUP:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_UP;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYHATMOTION:

					if (!SDLJoystick::IsAccelerometer (event->jhat.which)) {

						joystickEvent.type = JOYSTICK_HAT_MOVE;
						joystickEvent.index = event->jhat.hat;
						joystickEvent.eventValue = event->jhat.value;
						joystickEvent.id = event->jhat.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYDEVICEADDED:

					if (SDLJoystick::Connect (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_CONNECT;
						joystickEvent.id = SDLJoystick::GetInstanceID (event->jdevice.which);

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYDEVICEREMOVED:

					if (!SDLJoystick::IsAccelerometer (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_DISCONNECT;
						joystickEvent.id = event->jdevice.which;

						JoystickEvent::Dispatch (&joystickEvent);
						SDLJoystick::Disconnect (event->jdevice.which);

					}
					break;

			}

		}

	}


	void SDLApplication::ProcessKeyEvent (SDL_Event* event) {

		if (KeyEvent::callback) {

			switch (event->type) {

				case SDL_KEYDOWN: keyEvent.type = KEY_DOWN; break;
				case SDL_KEYUP: keyEvent.type = KEY_UP; break;

			}

			keyEvent.keyCode = event->key.keysym.sym;
			keyEvent.modifier = event->key.keysym.mod;
			keyEvent.windowID = event->key.windowID;

			if (keyEvent.type == KEY_DOWN) {

				if (keyEvent.keyCode == SDLK_CAPSLOCK) keyEvent.modifier |= KMOD_CAPS;
				if (keyEvent.keyCode == SDLK_LALT) keyEvent.modifier |= KMOD_LALT;
				if (keyEvent.keyCode == SDLK_LCTRL) keyEvent.modifier |= KMOD_LCTRL;
				if (keyEvent.keyCode == SDLK_LGUI) keyEvent.modifier |= KMOD_LGUI;
				if (keyEvent.keyCode == SDLK_LSHIFT) keyEvent.modifier |= KMOD_LSHIFT;
				if (keyEvent.keyCode == SDLK_MODE) keyEvent.modifier |= KMOD_MODE;
				if (keyEvent.keyCode == SDLK_NUMLOCKCLEAR) keyEvent.modifier |= KMOD_NUM;
				if (keyEvent.keyCode == SDLK_RALT) keyEvent.modifier |= KMOD_RALT;
				if (keyEvent.keyCode == SDLK_RCTRL) keyEvent.modifier |= KMOD_RCTRL;
				if (keyEvent.keyCode == SDLK_RGUI) keyEvent.modifier |= KMOD_RGUI;
				if (keyEvent.keyCode == SDLK_RSHIFT) keyEvent.modifier |= KMOD_RSHIFT;

			}

			KeyEvent::Dispatch (&keyEvent);

		}

	}


	void SDLApplication::ProcessMouseEvent (SDL_Event* event) {

		if (MouseEvent::callback) {

			switch (event->type) {

				case SDL_MOUSEMOTION:

					mouseEvent.type = MOUSE_MOVE;
					mouseEvent.x = event->motion.x;
					mouseEvent.y = event->motion.y;
					mouseEvent.movementX = event->motion.xrel;
					mouseEvent.movementY = event->motion.yrel;
					break;

				case SDL_MOUSEBUTTONDOWN:

					SDL_CaptureMouse (SDL_TRUE);

					mouseEvent.type = MOUSE_DOWN;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					mouseEvent.clickCount = event->button.clicks;
					break;

				case SDL_MOUSEBUTTONUP:

					SDL_CaptureMouse (SDL_FALSE);

					mouseEvent.type = MOUSE_UP;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					mouseEvent.clickCount = event->button.clicks;
					break;

				case SDL_MOUSEWHEEL:

					mouseEvent.type = MOUSE_WHEEL;

					if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {

						mouseEvent.x = -event->wheel.x;
						mouseEvent.y = -event->wheel.y;

					} else {

						mouseEvent.x = event->wheel.x;
						mouseEvent.y = event->wheel.y;

					}
					break;

			}

			mouseEvent.windowID = event->button.windowID;
			MouseEvent::Dispatch (&mouseEvent);

		}

	}


	void SDLApplication::ProcessSensorEvent (SDL_Event* event) {

		if (SensorEvent::callback) {

			double value = event->jaxis.value / 32767.0f;

			switch (event->jaxis.axis) {

				case 0: sensorEvent.x = value; break;
				case 1: sensorEvent.y = value; break;
				case 2: sensorEvent.z = value; break;
				default: break;

			}

			SensorEvent::Dispatch (&sensorEvent);

		}

	}


void SDLApplication::ProcessTextEvent (SDL_Event* event) {

    if (TextEvent::callback) {

        switch (event->type) {

            case SDL_TEXTINPUT:

                textEvent.type = TEXT_INPUT;
                break;

            case SDL_TEXTEDITING:

                textEvent.type = TEXT_EDIT;
                textEvent.start = event->edit.start;
                textEvent.length = event->edit.length;
                break;

            case SDL_TEXTEDITING_EXT:

                textEvent.type = TEXT_EDIT;
                textEvent.start = event->editExt.start;
                textEvent.length = event->editExt.length;
                if (textEvent.text) {
                    free(textEvent.text);
                }
                {
                    const char* extText = event->editExt.text;
                    size_t n = strlen(extText);
                    textEvent.text = (vbyte*)malloc(n + 1);
                    memcpy(textEvent.text, extText, n + 1);
                    SDL_free(event->editExt.text);
                }
                textEvent.windowID = event->editExt.windowID;
                TextEvent::Dispatch(&textEvent);
                return;

        }

        if (textEvent.text) {

            free (textEvent.text);

        }

        textEvent.text = (vbyte*)malloc (strlen (event->text.text) + 1);
        strcpy ((char*)textEvent.text, event->text.text);

        textEvent.windowID = event->text.windowID;
        TextEvent::Dispatch (&textEvent);

    }

}


	void SDLApplication::ProcessTouchEvent (SDL_Event* event) {

		if (TouchEvent::callback) {

			switch (event->type) {

				case SDL_FINGERMOTION:

					touchEvent.type = TOUCH_MOVE;
					break;

				case SDL_FINGERDOWN:

					touchEvent.type = TOUCH_START;
					break;

				case SDL_FINGERUP:

					touchEvent.type = TOUCH_END;
					break;

			}

			touchEvent.x = event->tfinger.x;
			touchEvent.y = event->tfinger.y;
			touchEvent.id = event->tfinger.fingerId;
			touchEvent.dx = event->tfinger.dx;
			touchEvent.dy = event->tfinger.dy;
			touchEvent.pressure = event->tfinger.pressure;
			touchEvent.device = event->tfinger.touchId;

			TouchEvent::Dispatch (&touchEvent);

		}

	}


	void SDLApplication::ProcessWindowEvent (SDL_Event* event) {
		const auto timelineNow = FrameTimeline::Now();
		if (timelineNow) {
			const SDL_WindowEvent& window = event->window;
			auto* nativeWindow = SDL_GetWindowFromID(window.windowID);
			switch (window.event) {
				case SDL_WINDOWEVENT_FOCUS_GAINED:
				case SDL_WINDOWEVENT_FOCUS_LOST:
					FrameTimeline::Write({"window_focus", 0, timelineNow, timelineNow,
						window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? 1u : 0u,
						nativeWindow ? SDL_GetWindowFlags(nativeWindow) : 0u});
					break;
				case SDL_WINDOWEVENT_SIZE_CHANGED:
					FrameTimeline::WindowGeometry(nativeWindow);
					break;
				default: break;
			}
		}

		if (WindowEvent::callback) {

			switch (event->window.event) {

				case SDL_WINDOWEVENT_SHOWN: windowEvent.type = WINDOW_SHOW; break;
				case SDL_WINDOWEVENT_CLOSE: windowEvent.type = WINDOW_CLOSE; break;
				case SDL_WINDOWEVENT_HIDDEN: windowEvent.type = WINDOW_HIDE; break;
				case SDL_WINDOWEVENT_ENTER: windowEvent.type = WINDOW_ENTER; break;
				case SDL_WINDOWEVENT_FOCUS_GAINED: windowEvent.type = WINDOW_FOCUS_IN; break;
				case SDL_WINDOWEVENT_FOCUS_LOST: windowEvent.type = WINDOW_FOCUS_OUT; break;
				case SDL_WINDOWEVENT_LEAVE: windowEvent.type = WINDOW_LEAVE; break;
				case SDL_WINDOWEVENT_MAXIMIZED: windowEvent.type = WINDOW_MAXIMIZE; break;
				case SDL_WINDOWEVENT_MINIMIZED: windowEvent.type = WINDOW_MINIMIZE; break;
				case SDL_WINDOWEVENT_EXPOSED: windowEvent.type = WINDOW_EXPOSE; break;

				case SDL_WINDOWEVENT_MOVED:

					windowEvent.type = WINDOW_MOVE;
					windowEvent.x = event->window.data1;
					windowEvent.y = event->window.data2;
					break;

				case SDL_WINDOWEVENT_SIZE_CHANGED:

					windowEvent.type = WINDOW_RESIZE;
					windowEvent.width = event->window.data1;
					windowEvent.height = event->window.data2;
					break;

				case SDL_WINDOWEVENT_RESTORED: windowEvent.type = WINDOW_RESTORE; break;

			}

			windowEvent.windowID = event->window.windowID;
			WindowEvent::Dispatch (&windowEvent);

		}

	}


	int SDLApplication::Quit () {

		applicationEvent.type = EXIT;
		ApplicationEvent::Dispatch (&applicationEvent);

		AndroidPerformanceHints::SetForeground(false);
		SDLWindow::StopRendering();
		FrameTimeline::Flush();
		SDL_QuitSubSystem (initFlags);

		SDL_Quit ();

		return 0;

	}


	void SDLApplication::RegisterWindow (SDLWindow *window) {

		#ifdef IPHONE
		SDL_iPhoneSetAnimationCallback (window->sdlWindow, 1, UpdateFrame, NULL);
		#endif

	}


	void SDLApplication::SetFrameRate (double frameRate) {

		#ifdef _WIN32
		frameRate = FrameRateOverride ("NOVAFLARE_UPDATE_RATE", frameRate);
		#endif
		framePeriod = frameRate > 0 ? 1000.0 / frameRate : 1000.0;
		if (performanceFrequency) {
			updatePeriodTicks = FramePeriodTicks (framePeriod, performanceFrequency);
			nextUpdateCounter = SDL_GetPerformanceCounter () + updatePeriodTicks;
		}
		TraceFrameSetting("update_period_ns", static_cast<std::uint64_t>(framePeriod * 1000000.0 + .5));

	}


	void SDLApplication::SetRenderFrameRate (double frameRate) {

		#ifdef _WIN32
		frameRate = FrameRateOverride ("NOVAFLARE_DRAW_RATE", frameRate);
		#endif
		renderFramePeriod = frameRate > 0 ? 1000.0 / frameRate : 1000.0;
#ifdef ANDROID
		renderRetryRequested = false;
#endif
		if (performanceFrequency) {
			renderPeriodTicks = FramePeriodTicks (renderFramePeriod, performanceFrequency);
			nextRenderCounter = SDL_GetPerformanceCounter () + renderPeriodTicks;
		}
		TraceFrameSetting("render_period_ns", static_cast<std::uint64_t>(renderFramePeriod * 1000000.0 + .5));

	}


	void SDLApplication::SetLockRender (bool split) {

		#ifdef _WIN32
		const char* overrideValue = std::getenv ("NOVAFLARE_SPLIT_RENDER");
		if (overrideValue != NULL) {
			if (overrideValue[0] == '1' && overrideValue[1] == '\0') split = true;
			if (overrideValue[0] == '0' && overrideValue[1] == '\0') split = false;
		}
		#endif
		lockRender = split;
#ifdef ANDROID
		renderRetryRequested = false;
#endif
		TraceFrameSetting("split_render", split ? 1 : 0);
		if (split && performanceFrequency) nextRenderCounter = SDL_GetPerformanceCounter () + renderPeriodTicks;

	}


	bool SDLApplication::Update () {

		#if defined(IPHONE) || defined(EMSCRIPTEN)
		int stackTop = 0;
		gc_set_top_of_stack (&stackTop, false);
		#endif

		if (!active) return false;

		SDL_Event event;
		while (SDL_PollEvent (&event)) {
			HandleEvent (&event);
			if (!active) return false;
		}

		if (!performanceFrequency) ResetFrameDeadlines ();
		if (!inBackground && !OpenGLBindings::CanRender()) SDLWindow::RetryContextRecovery();
		DispatchPendingRenderDeviceReset ();

		auto now = SDL_GetPerformanceCounter ();
		const bool updateDue = now >= nextUpdateCounter;
		if (updateDue) {
			// Publish the next deadline before callbacks so a rate change inside
			// UPDATE or RENDER_UPDATE can replace it.
			nextUpdateCounter += updatePeriodTicks;
			// Allow one missed update to run on the next pass through the event loop.
			if (now >= nextUpdateCounter && now - nextUpdateCounter >= updatePeriodTicks)
				nextUpdateCounter = now + updatePeriodTicks;
			if (!inBackground) DispatchScheduledUpdate ();
			if (!active) return false;
			now = SDL_GetPerformanceCounter ();
		}

		const bool renderDue = lockRender && now >= nextRenderCounter;
		if (renderDue) {
			nextRenderCounter = AdvanceFrameDeadline (nextRenderCounter, renderPeriodTicks, now);
			TryDispatchRender (renderFramePeriod);
		}

		if ((windowRedrawRequested
#ifdef ANDROID
			|| renderRetryRequested
#endif
			) && TryDispatchRender (lockRender ? renderFramePeriod : framePeriod)) return active;

		#if !defined(IPHONE) && !defined(EMSCRIPTEN)
		if (active && !updateDue && !renderDue) {
			const auto deadline = lockRender && nextRenderCounter < nextUpdateCounter
				? nextRenderCounter : nextUpdateCounter;
#ifdef ANDROID
			auto* thread = OpenGLBindings::renderThread;
			if (renderRetryRequested && lockRender && !inBackground &&
				OpenGLBindings::CanRender() && OpenGLBindings::isMultiThreaded &&
				thread && thread->IsRunning() && thread->CurrentSwapInterval() != 0) {
				const auto waitStart = SDL_GetPerformanceCounter();
				if (deadline > waitStart) {
					const auto nanoseconds = (deadline - waitStart) * 1000000000ULL / performanceFrequency;
					thread->WaitForPendingFrames(std::chrono::nanoseconds(nanoseconds));
				}
			} else
#endif
			WaitForFrameDeadline (deadline, performanceFrequency);
		}
		#endif

		return active;

	}


	void SDLApplication::UpdateFrame () {

		#ifdef EMSCRIPTEN
		System::GCTryExitBlocking ();
		#endif

		currentApplication->Update ();

		#ifdef EMSCRIPTEN
		System::GCTryEnterBlocking ();
		#endif

	}


	void SDLApplication::UpdateFrame (void*) {

		UpdateFrame ();

	}


	#if defined(_WIN32) || defined(HX_MACOS)
	int SDLCALL SDLApplication::HandleEventWatch (void *userdata, SDL_Event *event) {

		SDLApplication *app = (SDLApplication*)userdata;

		if (event->type == SDL_WINDOWEVENT && (
			event->window.event == SDL_WINDOWEVENT_RESIZED ||
			event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
			event->window.event == SDL_WINDOWEVENT_MOVED ||
			event->window.event == SDL_WINDOWEVENT_EXPOSED)) {
			/*
			SDL_Event userEvent;
			userEvent.type = SDL_USEREVENT;
			userEvent.user.code = 0;
			userEvent.user.data1 = NULL;
			userEvent.user.data2 = NULL;
			app->HandleEvent (&userEvent);
			*/

		}

		return 0;

	}
	#endif


	int SDLApplication::WaitEvent (SDL_Event *event) {

		#if defined(HX_MACOS)

		System::GCEnterBlocking ();
		int result = SDL_WaitEvent (event);
		System::GCExitBlocking ();
		return result;

		#elif defined(ANDROID)

		System::GCEnterBlocking ();
		int result = SDL_PollEvent (event);
		System::GCExitBlocking ();
		return result;

		#else

		bool isBlocking = false;

		for(;;) {

			SDL_PumpEvents ();

			switch (SDL_PeepEvents (event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {

				case -1:

					if (isBlocking) System::GCExitBlocking ();
					return 0;

				case 1:

					if (isBlocking) System::GCExitBlocking ();
					return 1;

				default:

					if (!isBlocking) System::GCEnterBlocking ();
					isBlocking = true;
					LIME_PAUSE();
					SDL_Delay (1);
					break;

			}

		}

		#endif

	}


	Application* CreateApplication () {

		return new SDLApplication ();

	}


}


#ifdef ANDROID
int SDL_main (int argc, char *argv[]) { return 0; }
#endif
