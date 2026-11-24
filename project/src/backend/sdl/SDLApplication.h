#ifndef LIME_SDL_APPLICATION_H
#define LIME_SDL_APPLICATION_H


#include <SDL.h>
#include <app/Application.h>
#include <app/ApplicationEvent.h>
#include <graphics/RenderEvent.h>
#include <system/ClipboardEvent.h>
#include <system/OrientationEvent.h>
#include <system/SensorEvent.h>
#include <ui/DropEvent.h>
#include <ui/GamepadEvent.h>
#include <ui/JoystickEvent.h>
#include <ui/KeyEvent.h>
#include <ui/MouseEvent.h>
#include <ui/TextEvent.h>
#include <ui/TouchEvent.h>
#include <ui/WindowEvent.h>
#include "SDLWindow.h"


namespace lime {


	class SDLApplication : public Application {

		public:

			SDLApplication ();
			~SDLApplication ();

			virtual int Exec ();
			virtual void Init ();
			virtual int Quit ();
			virtual void SetFrameRate (double frameRate);
			virtual void SetRenderFrameRate (double frameRate);
			virtual void SetLockRender (bool split);
			virtual bool Update ();

			void RegisterWindow (SDLWindow *window);

		private:

			void ResetFrameDeadlines ();
			void DispatchScheduledUpdate ();
			bool TryDispatchRender (double periodMs);
			void DispatchPendingRenderDeviceReset ();
			void HandleEvent (SDL_Event* event);
			void ProcessClipboardEvent (SDL_Event* event);
			void ProcessDropEvent (SDL_Event* event);
			void ProcessGamepadEvent (SDL_Event* event);
			void ProcessJoystickEvent (SDL_Event* event);
			void ProcessKeyEvent (SDL_Event* event);
			void ProcessMouseEvent (SDL_Event* event);
			#if defined(HX_ANDROID) || defined(IPHONE)
			bool SuppressTouchMouseEvent (const SDL_Event& event);
			bool suppressedTouchMouseDown = false;
			#endif
			void ProcessSensorEvent (SDL_Event* event);
			void ProcessTextEvent (SDL_Event* event);
			void ProcessTouchEvent (SDL_Event* event);
			void ProcessWindowEvent (SDL_Event* event);
			int WaitEvent (SDL_Event* event);

			static void UpdateFrame ();
			static void UpdateFrame (void*);
			#if defined(_WIN32) || defined(HX_MACOS)
			static int SDLCALL HandleEventWatch (void *userdata, SDL_Event *event);
			#endif

			static SDLApplication* currentApplication;

			bool active = false;
			bool lockRender;
			bool windowRedrawRequested = false;
			bool renderDeviceResetPending = false;
			ApplicationEvent applicationEvent;
			ClipboardEvent clipboardEvent;
			double framePeriod;
			double renderFramePeriod;
			Uint64 performanceFrequency = 0;
			Uint64 updatePeriodTicks = 0;
			Uint64 renderPeriodTicks = 0;
			Uint64 nextUpdateCounter = 0;
			Uint64 nextRenderCounter = 0;
			Uint32 initFlags;
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

	};


}


#endif
