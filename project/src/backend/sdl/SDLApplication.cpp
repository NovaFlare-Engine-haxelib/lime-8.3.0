#include "SDLApplication.h"
#include "SDLWindow.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include <graphics/RenderThread.h>
#include <system/System.h>
#include <map>
#include <stdint.h>

#ifdef HX_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef _WIN32
#include <timeapi.h>
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


	AutoGCRoot* Application::callback = 0;
	SDLApplication* SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	static std::map<int, std::map<int, int>> gamepadsAxisMap;
	static double accumulator = 0.0;
	static double accumulatorRender = 0.0;
	bool inBackground = false;
	static uint64_t perfFreq = 0;
	static uint64_t lastPerfCounter = 0;
	static uint64_t nextPerfCounter = 0;
	static uint64_t periodPerfTicks = 0;
	static uint64_t lastRenderPerfCounter = 0;
	static uint64_t nextRenderPerfCounter = 0;
	static uint64_t periodRenderPerfTicks = 0;
	static bool hrInit = false;


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
		framePeriod = 1000.0 / 60.0;
		renderFramePeriod = 1000.0 / 60.0;
		lockRender = false;

		currentUpdate = 0;
		lastUpdate = 0;
		nextUpdate = 0;

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


	void SDLApplication::HandleEvent (SDL_Event* event) {

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		int top = 0;
		gc_set_top_of_stack(&top,false);

		#endif

		switch (event->type) {

			case SDL_USEREVENT:

				if (!inBackground) {
					if (!hrInit) {
						perfFreq = SDL_GetPerformanceFrequency ();
						lastPerfCounter = SDL_GetPerformanceCounter ();
						periodPerfTicks = (uint64_t)(framePeriod * (double)perfFreq / 1000.0);
						nextPerfCounter = lastPerfCounter + periodPerfTicks;
						lastRenderPerfCounter = lastPerfCounter;
						periodRenderPerfTicks = (uint64_t)(renderFramePeriod * (double)perfFreq / 1000.0);
						nextRenderPerfCounter = lastRenderPerfCounter + periodRenderPerfTicks;
						hrInit = true;
					}

					uint64_t nowPerf = SDL_GetPerformanceCounter ();

					if (event->user.code == 0) { // Update
						double realDeltaTime = (double)(nowPerf - lastPerfCounter) * 1000.0 / (double)perfFreq;
						const double MAX_DELTA_TIME = 10 * framePeriod;
						if (realDeltaTime < 0) realDeltaTime = 0;
						if (realDeltaTime > MAX_DELTA_TIME) realDeltaTime = MAX_DELTA_TIME;
						accumulator += realDeltaTime;

						if (accumulator >= framePeriod) {
							lastPerfCounter = nowPerf;
							lastUpdate = (double)nowPerf * 1000.0 / (double)perfFreq;

							renderEvent.type = RENDER_UPDATE;
							RenderEvent::Dispatch (&renderEvent);

							applicationEvent.type = UPDATE;
							applicationEvent.deltaTime = realDeltaTime;
							ApplicationEvent::Dispatch (&applicationEvent);

							if (!lockRender) {
								if (RenderThread::activePendingFrames <= 0) {
									renderEvent.type = RENDER;
									RenderEvent::Dispatch (&renderEvent);
								} else {
									RenderThread::hasPendingRenderRequest = true;
								}
							}

							accumulator -= framePeriod;
							if (accumulator > framePeriod * 2) accumulator = 0;
						}

					} else if (event->user.code == 1) { // Render
						if (lockRender) {
							if (RenderThread::activePendingFrames > 0) {
								RenderThread::hasPendingRenderRequest = true;
								break;
							}
						
							double realDeltaTime = (double)(nowPerf - lastRenderPerfCounter) * 1000.0 / (double)perfFreq;
							const double MAX_DELTA_TIME = 10 * renderFramePeriod;
							if (realDeltaTime < 0) realDeltaTime = 0;
							if (realDeltaTime > MAX_DELTA_TIME) realDeltaTime = MAX_DELTA_TIME;
							accumulatorRender += realDeltaTime;

							if (accumulatorRender >= renderFramePeriod) {
								lastRenderPerfCounter = nowPerf;

								renderEvent.type = RENDER;
								RenderEvent::Dispatch (&renderEvent);

								accumulatorRender -= renderFramePeriod;
								if (accumulatorRender > renderFramePeriod * 2) accumulatorRender = 0;
							}
						}

					}
				}
				
				break;

			case SDL_APP_WILLENTERBACKGROUND:

				inBackground = true;
				//SDLWindow::PauseRendering();

				windowEvent.type = WINDOW_DEACTIVATE;
				WindowEvent::Dispatch (&windowEvent);
				break;

			case SDL_APP_WILLENTERFOREGROUND:

				break;

			case SDL_APP_DIDENTERFOREGROUND:

				SDLWindow::ResumeRendering();
				windowEvent.type = WINDOW_ACTIVATE;
				WindowEvent::Dispatch (&windowEvent);

				inBackground = false;
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
			case SDL_MOUSEWHEEL:

				ProcessMouseEvent (event);
				break;

			#ifndef EMSCRIPTEN
			case SDL_RENDER_DEVICE_RESET:

				renderEvent.type = RENDER_CONTEXT_LOST;
				RenderEvent::Dispatch (&renderEvent);

				renderEvent.type = RENDER_CONTEXT_RESTORED;
				RenderEvent::Dispatch (&renderEvent);

				renderEvent.type = RENDER;
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

						ProcessWindowEvent (event);

						if (!inBackground) {
							RenderEvent::Dispatch (&renderEvent);
						}

						break;

					case SDL_WINDOWEVENT_SIZE_CHANGED:

						ProcessWindowEvent (event);

						if (!inBackground) {
							RenderEvent::Dispatch (&renderEvent);
						}

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
		if (!hrInit) {
			perfFreq = SDL_GetPerformanceFrequency ();
			hrInit = true;
		}
		uint64_t now = SDL_GetPerformanceCounter ();
		lastPerfCounter = now;
		nextPerfCounter = now + periodPerfTicks;
		lastRenderPerfCounter = now;
		nextRenderPerfCounter = now + periodRenderPerfTicks;
		
		double nowMs = (double)now * 1000.0 / (double)perfFreq;
		lastUpdate = nowMs;
		currentUpdate = nowMs;
		nextUpdate = nowMs + framePeriod;

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

		accumulator = 0.0;

		if (frameRate > 0) {

			framePeriod = 1000.0 / frameRate;
			if (perfFreq) periodPerfTicks = (uint64_t)(framePeriod * (double)perfFreq / 1000.0);

		} else {

			framePeriod = 1000.0;
			if (perfFreq) periodPerfTicks = (uint64_t)(framePeriod * (double)perfFreq / 1000.0);

		}
	}


	void SDLApplication::SetRenderFrameRate (double frameRate) {

		accumulatorRender = 0.0;

		if (frameRate > 0) {

			renderFramePeriod = 1000.0 / frameRate;
			if (perfFreq) periodRenderPerfTicks = (uint64_t)(renderFramePeriod * (double)perfFreq / 1000.0);

		} else {

			renderFramePeriod = 1000.0;
			if (perfFreq) periodRenderPerfTicks = (uint64_t)(renderFramePeriod * (double)perfFreq / 1000.0);

		}

	}


	void SDLApplication::SetLockRender (bool split) {

		lockRender = split;
		if (split) {
			accumulatorRender = 0;
		}

	}


	static SDL_TimerID timerID = 0;
	bool timerActive = false;
	bool firstTime = true;

	Uint32 OnTimer (Uint32 interval, void *) {

		SDL_Event event;
		SDL_UserEvent userevent;
		userevent.type = SDL_USEREVENT;
		userevent.code = 0;
		userevent.data1 = NULL;
		userevent.data2 = NULL;
		event.type = SDL_USEREVENT;
		event.user = userevent;

		timerActive = false;
		timerID = 0;

		SDL_PushEvent (&event);

		return 0;

	}


	bool SDLApplication::Update () {

		SDL_Event event;
		event.type = -1;

		#if (!defined (IPHONE) && !defined (EMSCRIPTEN))

		if (active) {
			if (firstTime) {
				firstTime = false;
			} else {
				if (SDL_PollEvent (&event)) {
					HandleEvent (&event);
					event.type = -1;
					if (!active)
						return active;
				}
			}

		#endif

		while (SDL_PollEvent (&event)) {

			HandleEvent (&event);
			event.type = -1;
			if (!active)
				return active;

		}
		
		if (!hrInit) {
			perfFreq = SDL_GetPerformanceFrequency ();
			lastPerfCounter = SDL_GetPerformanceCounter ();
			periodPerfTicks = (uint64_t)(framePeriod * (double)perfFreq / 1000.0);
			nextPerfCounter = lastPerfCounter + periodPerfTicks;
			hrInit = true;
		}

		uint64_t nowPerf = SDL_GetPerformanceCounter ();
		double nowMs = (double)nowPerf * 1000.0 / (double)perfFreq;
		currentUpdate = (Uint32)nowMs;

		#if defined (IPHONE) || defined (EMSCRIPTEN)

			if (nowMs >= nextUpdate) {

				event.type = SDL_USEREVENT;
				HandleEvent (&event);
				event.type = -1;

			}

		#else
			bool updatePending = (nowPerf >= nextPerfCounter);
			bool renderPending = (nowPerf >= nextRenderPerfCounter);

			if (updatePending) {
				SDL_Event ev;
				ev.type = SDL_USEREVENT;
				ev.user.code = 0;
				ev.user.data1 = NULL;
				ev.user.data2 = NULL;
				
				HandleEvent (&ev);
				
				nextPerfCounter += periodPerfTicks;
				if (nextPerfCounter + periodPerfTicks < nowPerf) {
					nextPerfCounter = nowPerf + periodPerfTicks;
				}
			}

			if (renderPending) {
				SDL_Event ev;
				ev.type = SDL_USEREVENT;
				ev.user.code = 1;
				ev.user.data1 = NULL;
				ev.user.data2 = NULL;
				
				HandleEvent (&ev);
				
				nextRenderPerfCounter += periodRenderPerfTicks;
				if (nextRenderPerfCounter + periodRenderPerfTicks < nowPerf) {
					nextRenderPerfCounter = nowPerf + periodRenderPerfTicks;
				}
			}

			if (!updatePending && !renderPending) {
				uint64_t nextEventCounter = (nextPerfCounter < nextRenderPerfCounter) ? nextPerfCounter : nextRenderPerfCounter;
				if (nextEventCounter > nowPerf) {
					uint64_t remainingTicks = nextEventCounter - nowPerf;
					if (remainingTicks > perfFreq / 500) {
						SDL_Delay (1);
					} else {
						LIME_PAUSE();
					}
				}
			}
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

			SDL_Event userEvent;
			userEvent.type = SDL_USEREVENT;
			userEvent.user.code = 0;
			userEvent.user.data1 = NULL;
			userEvent.user.data2 = NULL;
			app->HandleEvent (&userEvent);

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
