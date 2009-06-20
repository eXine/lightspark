/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#define GL_GLEXT_PROTOTYPES
#include <iostream>
#include <string.h>
#include <pthread.h>
#include <SDL/SDL.h>
#include <algorithm>

#include "flashdisplay.h"
#include "flashevents.h"
#include "swf.h"
#include "logger.h"
#include "actions.h"
#include "streams.h"
#include "asobjects.h"
#include "textfile.h"

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
using namespace std;

int ParseThread::error(0);

list<IDisplayListElem*> null_list;
int RenderThread::error(0);

extern __thread SystemState* sys;

SWF_HEADER::SWF_HEADER(istream& in)
{
	//Valid only on little endian platforms
	in >> Signature[0] >> Signature[1] >> Signature[2];

	in >> Version >> FileLength;
	if(Signature[0]=='F' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(NO_INFO, "Uncompressed SWF file: Version " << (int)Version << " Length " << FileLength);
	}
	else if(Signature[0]=='C' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(NO_INFO, "Compressed SWF file: Version " << (int)Version << " Length " << FileLength);
		swf_stream* ss=dynamic_cast<swf_stream*>(in.rdbuf());
		ss->setCompressed();
	}
	sys->version=Version;
	in >> FrameSize >> FrameRate >> FrameCount;
	LOG(NO_INFO,"FrameRate " << FrameRate);
	sys->frame_rate=FrameRate;
	sys->frame_rate/=256;
	sys->state.max_FP=FrameCount;
}

SystemState::SystemState():currentClip(this),parsingDisplayList(&displayList),performance_profiling(false),
	parsingTarget(this),shutdown(false),currentVm(NULL)
{
	sem_init(&sem_dict,0,1);
	sem_init(&new_frame,0,0);
	sem_init(&sem_run,0,0);
	sem_init(&sem_valid_frame_size,0,0);

	sem_init(&mutex,0,1);

	//Register global functions
	setVariableByName("parseInt",new Function(parseInt));

	//Register default objects
	ISWFObject* stage(new ASStage);
	setVariableByName("Stage",stage);

	ISWFObject* array(new ASArray);
	array->_register();
	setVariableByName("Array",array);

	ISWFObject* object(new ASObject);
	object->_register();
	setVariableByName("Object",object);

	ISWFObject* mcloader(new ASMovieClipLoader);
	setVariableByName("MovieClipLoader",mcloader);

	ISWFObject* xml(new ASXML);
	setVariableByName("XML",xml);

	setVariableByName("",this);

	//This should come from DisplayObject
	LoaderInfo* loaderInfo=new LoaderInfo(true);
	setVariableByName("loaderInfo",loaderInfo);
	//Setting fake parameters
	loaderInfo->parameters.setVariableByName(".debug_level",new ASString(""));
	loaderInfo->parameters.setVariableByName(".connect",new ASString("true"));
	loaderInfo->parameters.setVariableByName(".chat_ip",new ASString("216.246.59.237"));
	loaderInfo->parameters.setVariableByName(".chat_port",new ASString("5222"));
	loaderInfo->parameters.setVariableByName(".chat_host",new ASString("of1.kongregate.com"));
}

void SystemState::setShutdownFlag()
{
	sem_wait(&mutex);
	shutdown=true;
	if(sys->currentVm)
		sys->currentVm->addEvent(NULL,new ShutdownEvent());

	//Signal blocking semaphore
	sem_post(&sem_run);
	sem_post(&mutex);
}

void* ParseThread::worker(ParseThread* th)
{
	sys=th->m_sys;
	try
	{
		SWF_HEADER h(th->f);
		sys->setFrameSize(h.getFrameSize());
		sys->setFrameCount(h.FrameCount);

		int done=0;

		TagFactory factory(th->f);
		while(1)
		{
			if(error)
			{
				LOG(NO_INFO,"Terminating parsing thread on error state");
				pthread_exit(NULL);
			}
			Tag* tag=factory.readTag();
			switch(tag->getType())
			{
			//	case TAG:
				case END_TAG:
				{
					LOG(NO_INFO,"End of parsing @ " << th->f.tellg());
					pthread_exit(NULL);
				}
				case DICT_TAG:
					sys->addToDictionary(dynamic_cast<DictionaryTag*>(tag));
					break;
				case DISPLAY_LIST_TAG:
					sys->addToDisplayList(dynamic_cast<DisplayListTag*>(tag));
					break;
				case SHOW_TAG:
				{
					sys->commitFrame();
					break;
				}
				case CONTROL_TAG:
					dynamic_cast<ControlTag*>(tag)->execute();
					break;
			}
			if(sys->shutdown)
				pthread_exit(0);
		}
	}
	catch(const char* s)
	{
		LOG(ERROR,"Exception caught: " << s);
		exit(-1);
	}
}

ParseThread::ParseThread(SystemState* s,istream& in):f(in)
{
	m_sys=s;
	error=0;
	pthread_create(&t,NULL,(thread_worker)worker,this);
}

ParseThread::~ParseThread()
{
	void* ret;
	pthread_cancel(t);
	pthread_join(t,&ret);
}

void RenderThread::wait()
{
	pthread_cancel(t);
	pthread_join(t,NULL);
}

void ParseThread::wait()
{
	pthread_join(t,NULL);
}

InputThread::InputThread(SystemState* s,ENGINE e, void* param)
{
	m_sys=s;
	LOG(NO_INFO,"Creating input thread");
	sem_init(&sem_listeners,0,1);
	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
	else
	{
		npapi_params=(NPAPI_params*)param;
		pthread_create(&t,NULL,(thread_worker)npapi_worker,this);
	}
}

InputThread::~InputThread()
{
	void* ret;
	pthread_cancel(t);
	pthread_join(t,&ret);
}

void InputThread::wait()
{
	pthread_join(t,NULL);
}

void* InputThread::npapi_worker(InputThread* th)
{
	sys=th->m_sys;
/*	NPAPI_params* p=(NPAPI_params*)in_ptr;
//	Display* d=XOpenDisplay(NULL);
	XSelectInput(p->display,p->window,PointerMotionMask|ExposureMask);

	XEvent e;
	while(XWindowEvent(p->display,p->window,PointerMotionMask|ExposureMask, &e))
	{
		exit(-1);
	}*/
}

void* InputThread::sdl_worker(InputThread* th)
{
	sys=th->m_sys;
	SDL_Event event;
	while(SDL_WaitEvent(&event))
	{
		switch(event.type)
		{
			case SDL_KEYDOWN:
			{
				switch(event.key.keysym.sym)
				{
					case SDLK_q:
						sys->setShutdownFlag();
						pthread_exit(0);
						break;
					case SDLK_s:
						sys->state.stop_FP=true;
						break;
					case SDLK_o:
						sys->displayListLimit--;
						break;
					case SDLK_p:
						sys->displayListLimit++;
						break;
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			{
				sem_wait(&th->sem_listeners);
				
				if(!th->listeners.empty())
				{
					cout << "listeners " << th->listeners.size() << endl;
					//sys->currentVm->addEvent(th->listeners[0],NULL);
					sys->dumpEvents();
				}

				sem_post(&th->sem_listeners);
				break;
			}
		}
	}
}

void InputThread::addListener(const string& t, InteractiveObject* ob)
{
	sem_wait(&sem_listeners);
	LOG(TRACE,"Adding listener to " << t);
	pair< map<string, InteractiveObject*>::iterator,map<string, InteractiveObject*>::iterator > range=listeners.equal_range(t);

	//Check if this object is alreasy registered for this event
	for(range.first;range.first!=range.second;range.first++)
	{
		if(range.first->second==ob)
		{
			LOG(TRACE,"Already added");
			sem_post(&sem_listeners);
			return;
		}
	}

	//Register the listener
	listeners.insert(make_pair(t,ob));

	sem_post(&sem_listeners);
}

void InputThread::broadcastEvent(const string& t)
{
	sem_wait(&sem_listeners);

	pair< map<string,InteractiveObject*>::iterator,map<string, InteractiveObject*>::iterator > range=
		listeners.equal_range(t);

	for(range.first;range.first!=range.second;range.first++)
		sys->currentVm->addEvent(range.first->second,new Event(t));

	sem_post(&sem_listeners);
}

RenderThread::RenderThread(SystemState* s,ENGINE e,void* params):bak_frame(null_list)
{
	LOG(NO_INFO,"RenderThread this=" << this);
	m_sys=s;
	sem_init(&mutex,0,1);
	sem_init(&render,0,0);
	sem_init(&end_render,0,0);
	error=0;
	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
	else if(e==NPAPI)
	{
		npapi_params=(NPAPI_params*)params;
		pthread_create(&t,NULL,(thread_worker)npapi_worker,this);
	}
}

RenderThread::~RenderThread()
{
	LOG(NO_INFO,"~RenderThread this=" << this);
	void* ret;
	pthread_cancel(t);
	pthread_join(t,&ret);
}

void* RenderThread::npapi_worker(RenderThread* th)
{
	sys=th->m_sys;
	NPAPI_params* p=th->npapi_params;
	
	Display* d=XOpenDisplay(NULL);

	XFontStruct *mFontInfo;
	if (!mFontInfo)
	{
		if (!(mFontInfo = XLoadQueryFont(d, "9x15")))
			printf("Cannot open 9X15 font\n");
	}

	XGCValues v;
	v.foreground=BlackPixel(d, 0);
	v.background=WhitePixel(d, 0);
	v.font=XLoadFont(d,"9x15");
	th->mGC=XCreateGC(d,p->window,GCForeground|GCBackground|GCFont,&v);

    	int a,b;
    	Bool glx_present=glXQueryVersion(d,&a,&b);
	if(!glx_present)
	{
		printf("glX not present\n");
		return NULL;
	}
	int attrib[10];
	attrib[0]=GLX_BUFFER_SIZE;
	attrib[1]=24;
	attrib[2]=GLX_VISUAL_ID;
	attrib[3]=p->visual;
	attrib[4]=GLX_DEPTH_SIZE;
	attrib[5]=24;
	attrib[6]=GLX_STENCIL_SIZE;
	attrib[7]=8;

	attrib[8]=0;
	GLXFBConfig* fb=glXChooseFBConfig(d, 0, attrib, &a);
//	printf("returned %x pointer and %u elements\n",fb, a);
	if(!fb)
	{
		attrib[0]=0;
		fb=glXChooseFBConfig(d, 0, NULL, &a);
		LOG(ERROR,"Falling back to no depth and no stencil");
	}
	int i;
	for(i=0;i<a;i++)
	{
		int id,v;
		glXGetFBConfigAttrib(d,fb[i],GLX_BUFFER_SIZE,&v);
		glXGetFBConfigAttrib(d,fb[i],GLX_VISUAL_ID,&id);
//		printf("ID 0x%x size %u\n",id,v);
		if(id==p->visual)
		{
//			printf("good id %x\n",id);
			break;
		}
	}
	th->mFBConfig=fb[i];
	XFree(fb);

	th->mContext = glXCreateNewContext(d,th->mFBConfig,GLX_RGBA_TYPE ,NULL,1);
	glXMakeContextCurrent(d, p->window, p->window, th->mContext);
	if(!glXIsDirect(d,th->mContext))
		printf("Indirect!!\n");


	glViewport(0,0,p->width,p->height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,p->width,p->height,0,-100,0);
	glMatrixMode(GL_MODELVIEW);

	try
	{
		while(1)
		{
			sem_wait(&th->render);
			if(error)
			{
				glXMakeContextCurrent(d, 0, 0, NULL);
				unsigned int h = p->height/2;
				unsigned int w = 3 * p->width/4;
				int x = 0;
				int y = h/2;
				GC gc = XCreateGC(d, p->window, 0, NULL);
				const char *string = "ERROR";
				int l = strlen(string);
				int fmba = mFontInfo->max_bounds.ascent;
				int fmbd = mFontInfo->max_bounds.descent;
				int fh = fmba + fmbd;
				y += fh;
				x += 32;
				XDrawString(d, p->window, gc, x, y, string, l);
				XFreeGC(d, gc);
			}
			else
			{
				sem_wait(&th->mutex);
				if(th->cur_frame==NULL)
				{
					sem_post(&th->mutex);
					sem_post(&th->end_render);
					continue;
				}
				RGB bg=sys->getBackground();
				glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,0);
				glClearDepth(0xffff);
				glClearStencil(5);
				glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
				glLoadIdentity();

				float scalex=p->width;
				scalex/=sys->getFrameSize().Xmax;
				float scaley=p->height;
				scaley/=sys->getFrameSize().Ymax;
				glScalef(scalex,scaley,1);

				th->cur_frame->Render(0);
				glFlush();
				glXSwapBuffers(d,p->window);
				sem_post(&th->mutex);
			}
			sem_post(&th->end_render);
			if(sys->shutdown)
				pthread_exit(0);
		}
	}
	catch(const char* e)
	{
		LOG(ERROR,"Exception caught " << e);
		exit(-1);
	}
	delete p;
}

int RenderThread::load_fragment_program(const char* file)
{
	GLuint v,f,f2,p;
	f = glCreateShader(GL_FRAGMENT_SHADER);

	char *fs = NULL;
	fs = textFileRead(file);

	const char * ff = fs;
	glShaderSource(f, 1, &ff,NULL);
	free(fs);

	glCompileShader(f);
	char str[1024];
	int a;
	glGetShaderInfoLog(f,1024,&a,str);
	printf("%s\n",str);

	int ret = glCreateProgram();
	glAttachShader(ret,f);

	glLinkProgram(ret);
	return ret;
}

void* RenderThread::sdl_worker(RenderThread* th)
{
	sys=th->m_sys;
	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute( SDL_GL_SWAP_CONTROL, 0 );
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1); 

	RECT size=sys->getFrameSize();
	int width=size.Xmax/10;
	int height=size.Ymax/10;
	SDL_SetVideoMode( width, height, 24, SDL_OPENGL );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc(GL_LEQUAL);

//	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	glViewport(0,0,width,height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,width,height,0,-100,0);
	glScalef(0.1,0.1,1);

	glMatrixMode(GL_MODELVIEW);

	unsigned int t;
	glGenTextures(1,&t);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_1D,t);

	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_WRAP_S,GL_CLAMP);

	//Load fragment shaders
	sys->gpu_program=load_fragment_program("lightspark.frag");
	int tex=glGetUniformLocation(sys->gpu_program,"g_tex");
	glUniform1i(tex,0);
	glUseProgram(sys->gpu_program);

	float* buffer=new float[640*240];
	try
	{
		while(1)
		{
			sem_wait(&th->render);
			if(th->cur_frame==NULL)
			{
				sem_post(&th->end_render);
				if(sys->shutdown)
					pthread_exit(0);
				continue;
			}
			SDL_GL_SwapBuffers( );
			RGB bg=sys->getBackground();
			glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,0);
			glClearDepth(0xffff);
			glClearStencil(5);
			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
			glLoadIdentity();

			th->cur_frame->Render(sys->displayListLimit);

			/*glReadPixels(0,240,640,240,GL_STENCIL_INDEX,GL_FLOAT,buffer);
			for(int i=0;i<240*640;i++)
				buffer[i]/=4;
			glDrawPixels(640,240,GL_LUMINANCE,GL_FLOAT,buffer);*/

			sem_post(&th->end_render);
			if(sys->shutdown)
			{
				delete[] buffer;
				pthread_exit(0);
			}
		}
	}
	catch(const char* e)
	{
		LOG(ERROR, "Exception caught " << e);
		delete[] buffer;
		exit(-1);
	}
}

void RenderThread::draw(Frame* f)
{
	cur_frame=f;

	sys->cur_input_thread->broadcastEvent("enterFrame");
	sem_post(&render);
	sem_wait(&end_render);
	usleep(1000000/sys->frame_rate);

}

void SystemState::waitToRun()
{
	sem_wait(&mutex);

	if(state.stop_FP && !(update_request || performance_profiling))
	{
		cout << "stop" << endl;
		sem_post(&mutex);
		sem_wait(&sem_run);
		sem_wait(&mutex);
	}
	while(1)
	{
		if(state.FP<frames.size())
			break;

		sem_post(&mutex);
		sem_wait(&new_frame);
		sem_wait(&mutex);
	}
	update_request=false;
	if(!state.stop_FP)
		state.next_FP=state.FP+1;
	else
		state.next_FP=state.FP;
	if(state.next_FP>=state.max_FP)
	{
		state.next_FP=state.FP;
		//state.stop_FP=true;
	}
	sem_post(&mutex);
}

Frame& SystemState::getFrameAtFP() 
{
	sem_wait(&mutex);
	list<Frame>::iterator it=frames.begin();
	for(int i=0;i<state.FP;i++)
		it++;
	sem_post(&mutex);

	return *it;
}

void SystemState::advanceFP()
{
	sem_wait(&mutex);
	state.tick();
	sem_post(&mutex);
}

void SystemState::setFrameCount(int f)
{
	sem_wait(&mutex);
	_totalframes=f;
	sem_post(&mutex);
}

void SystemState::setFrameSize(const RECT& f)
{
	frame_size=f;
	sem_post(&sem_valid_frame_size);
}

RECT SystemState::getFrameSize()
{
	sem_wait(&sem_valid_frame_size);
	return frame_size;
}

void SystemState::addToDictionary(DictionaryTag* r)
{
	sem_wait(&mutex);
	//sem_wait(&sys.sem_dict);
	dictionary.push_back(r);
	//sem_post(&sys.sem_dict);
	sem_post(&mutex);
}

void SystemState::addToDisplayList(IDisplayListElem* t)
{
	sem_wait(&mutex);
	ASMovieClip::addToDisplayList(t);
	sem_post(&mutex);
}

void SystemState::commitFrame()
{
	sem_wait(&mutex);
	//sem_wait(&clip.sem_frames);
	frames.push_back(Frame(displayList));
	_framesloaded=frames.size();
	sem_post(&new_frame);
	sem_post(&mutex);
	//sem_post(&clip.sem_frames);
}

RGB SystemState::getBackground()
{
	return Background;
}

void SystemState::setBackground(const RGB& bg)
{
	Background=bg;
}

void SystemState::setUpdateRequest(bool s)
{
	sem_wait(&mutex);
	update_request=s;
	sem_post(&mutex);
}

DictionaryTag* SystemState::dictionaryLookup(int id)
{
	sem_wait(&mutex);
	//sem_wait(&sem_dict);
	list< DictionaryTag*>::iterator it = dictionary.begin();
	for(it;it!=dictionary.end();it++)
	{
		if((*it)->getId()==id)
			break;
	}
	if(it==dictionary.end())
		LOG(ERROR,"No such Id on dictionary " << id);
	//sem_post(&sem_dict);
	sem_post(&mutex);
	return *it;
}

ISWFObject* SystemState::getVariableByName(const string& name, bool& found)
{
	sem_wait(&mutex);
	ISWFObject* ret=ISWFObject::getVariableByName(name, found);
	sem_post(&mutex);
	return ret;
}

ISWFObject* SystemState::setVariableByName(const string& name, ISWFObject* o, bool force)
{
	sem_wait(&mutex);
	ISWFObject* ret=ISWFObject::setVariableByName(name,o,force);
	sem_post(&mutex);
	return ret;
}

SWFOBJECT_TYPE SystemState::getObjectType() const
{
	return T_MOVIE;
}

