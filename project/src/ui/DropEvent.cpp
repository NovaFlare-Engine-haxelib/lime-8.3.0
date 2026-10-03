#include <system/CFFI.h>
#include <ui/DropEvent.h>


namespace lime {


	ValuePointer* DropEvent::callback = 0;
	ValuePointer* DropEvent::eventObject = 0;

	static int id_file;
	static int id_type;
	static int id_windowID;
	static int id_x;
	static int id_y;
	static bool init = false;


	DropEvent::DropEvent () {

		file = 0;
		type = DROP_FILE;
		windowID=0;x=0;y=0;

	}


	void DropEvent::Dispatch (DropEvent* event) {

		if (DropEvent::callback) {

			if (DropEvent::eventObject->IsCFFIValue ()) {

				if (!init) {

					id_file = val_id ("file");
					id_type = val_id ("type");
					id_windowID=val_id("windowID");id_x=val_id("x");id_y=val_id("y");
					init = true;

				}

				value object = (value)DropEvent::eventObject->Get ();

				alloc_field (object, id_file, event->file?alloc_string((const char*)event->file):alloc_null());
				alloc_field (object, id_type, alloc_int (event->type));
				alloc_field(object,id_windowID,alloc_int(event->windowID));alloc_field(object,id_x,alloc_float(event->x));alloc_field(object,id_y,alloc_float(event->y));

			} else {

				DropEvent* eventObject = (DropEvent*)DropEvent::eventObject->Get ();

				int length = event->file?strlen((const char*)event->file):0;
				char* file = (char*)malloc (length + 1);
				if(event->file)strcpy(file,(const char*)event->file);else file[0]=0;
				eventObject->file = (vbyte*)file;
				eventObject->type = event->type;
				eventObject->windowID=event->windowID;eventObject->x=event->x;eventObject->y=event->y;

			}

			DropEvent::callback->Call ();

		}

	}


}