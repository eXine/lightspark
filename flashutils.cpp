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

#include "abc.h"
#include "flashutils.h"
#include "asobjects.h"
#include "class.h"
#include "compat.h"

using namespace std;
using namespace lightspark;

REGISTER_CLASS_NAME(ByteArray);
REGISTER_CLASS_NAME(Timer);
REGISTER_CLASS_NAME(Dictionary);

ByteArray::ByteArray():bytes(NULL),len(0),position(0)
{
}

ByteArray::~ByteArray()
{
	delete[] bytes;
}

void ByteArray::sinit(Class_base* c)
{
}

void ByteArray::buildTraits(ASObject* o)
{
	o->setGetterByQName("length","",new Function(_getLength));
	o->setGetterByQName("bytesAvailable","",new Function(_getBytesAvailable));
	o->setGetterByQName("position","",new Function(_getPosition));
	o->setSetterByQName("position","",new Function(_setPosition));
	o->setVariableByQName("readBytes","",new Function(readBytes));
}

uint8_t* ByteArray::getBuffer(unsigned int size)
{
	if(bytes==NULL)
	{
		len=size;
		bytes=new uint8_t[len];
	}
	else
	{
		assert(size<=len);
	}
	return bytes;
}

ASFUNCTIONBODY(ByteArray,_getPosition)
{
	ByteArray* th=static_cast<ByteArray*>(obj->implementation);
	return abstract_i(th->position);
}

ASFUNCTIONBODY(ByteArray,_setPosition)
{
	ByteArray* th=static_cast<ByteArray*>(obj->implementation);
	int pos=args->at(0)->toInt();
	th->position=pos;
	return NULL;
}

ASFUNCTIONBODY(ByteArray,_getLength)
{
	ByteArray* th=static_cast<ByteArray*>(obj->implementation);
	return abstract_i(th->len);
}

ASFUNCTIONBODY(ByteArray,_getBytesAvailable)
{
	ByteArray* th=static_cast<ByteArray*>(obj->implementation);
	return abstract_i(th->len-th->position);
}

ASFUNCTIONBODY(ByteArray,readBytes)
{
	ByteArray* th=static_cast<ByteArray*>(obj->implementation);
	//Validate parameters
	assert(args->size()==3);
	assert(args->at(0)->prototype==Class<ByteArray>::getClass());

	ByteArray* out=Class<ByteArray>::cast(args->at(0)->implementation);
	int offset=args->at(1)->toInt();
	int length=args->at(2)->toInt();
	assert(offset==0);

	uint8_t* buf=out->getBuffer(length);
	memcpy(buf,th->bytes+th->position,length);
	th->position+=length;

	return NULL;
}

bool ByteArray::getVariableByMultiname(const multiname& name, ASObject*& out)
{
	int index=0;
	if(!Array::isValidMultiname(name,index))
		return false;

	assert(index<len);
	out=abstract_i(bytes[index]);

	return true;
}

bool ByteArray::getVariableByMultiname_i(const multiname& name, intptr_t& out)
{
	int index=0;
	if(!Array::isValidMultiname(name,index))
		return false;

	assert(index<len);
	out=bytes[index];

	return true;
}

bool ByteArray::setVariableByQName(const tiny_string& name, const tiny_string& ns, ASObject* o)
{
	int index=0;
	if(!Array::isValidQName(name,ns,index))
		return false;

	abort();
}

bool ByteArray::setVariableByMultiname(const multiname& name, ASObject* o)
{
	int index=0;
	if(!Array::isValidMultiname(name,index))
		return false;

	if(index>=len)
	{
		//Resize the array
		uint8_t* buf=new uint8_t[index+1];
		memcpy(buf,bytes,len);
		delete[] bytes;
		len=index+1;
		bytes=buf;
	}

	if(o->getObjectType()!=T_UNDEFINED)
		bytes[index]=o->toInt();
	else
		bytes[index]=0;
	return true;
}

bool ByteArray::setVariableByMultiname_i(const multiname& name, intptr_t value)
{
	int index=0;
	if(!Array::isValidMultiname(name,index))
		return false;

	abort();
}

bool ByteArray::isEqual(bool& ret, ASObject* r)
{
	if(r->getObjectType()!=T_OBJECT)
		return false;
	
	abort();
}

void Timer::execute()
{
	while(running)
	{
		compat_msleep(delay);
		if(running)
		{
			sys->currentVm->addEvent(this,Class<TimerEvent>::getInstanceS(true,"timer"));
			//Do not spam timer events until this is done
			SynchronizationEvent* se=new SynchronizationEvent;
			sys->currentVm->addEvent(NULL,se);
			se->wait();
			delete se;
		}
	}
}

void Timer::sinit(Class_base* c)
{
	assert(c->constructor==NULL);
	c->constructor=new Function(_constructor);
}

ASFUNCTIONBODY(Timer,_constructor)
{
	EventDispatcher::_constructor(obj,NULL);
	Timer* th=static_cast<Timer*>(obj->implementation);
	obj->setVariableByQName("start","",new Function(start));
	obj->setVariableByQName("reset","",new Function(reset));

	assert(args->size()==1);

	//Set only the delay for now
	th->delay=args->at(0)->toInt();
}

ASFUNCTIONBODY(Timer,start)
{
	Timer* th=static_cast<Timer*>(obj->implementation);
	th->running=true;
	sys->cur_thread_pool->addJob(th);
}

ASFUNCTIONBODY(Timer,reset)
{
	Timer* th=static_cast<Timer*>(obj->implementation);
	th->running=false;
}

ASObject* lightspark::getQualifiedClassName(ASObject* obj, arguments* args)
{
	//CHECK: what to do is ns is empty
	ASObject* target=args->at(0);
	Class_base* c;
	if(target->getObjectType()!=T_CLASS)
	{
		assert(target->prototype);
		c=target->prototype;
	}
	else
		c=static_cast<Class_base*>(target);

	return Class<ASString>::getInstanceS(true,c->getQualifiedClassName())->obj;
}

ASObject* lightspark::getQualifiedSuperclassName(ASObject* obj, arguments* args)
{
	//CHECK: what to do is ns is empty
	ASObject* target=args->at(0);
	Class_base* c;
	if(target->getObjectType()!=T_CLASS)
	{
		assert(target->prototype);
		c=target->prototype->super;
	}
	else
		c=static_cast<Class_base*>(target)->super;

	assert(c);

	cout << c->getQualifiedClassName() << endl;
	return Class<ASString>::getInstanceS(true,c->getQualifiedClassName())->obj;
}

ASFUNCTIONBODY(lightspark,getDefinitionByName)
{
	assert(args && args->size()==1);
	const tiny_string& tmp=args->at(0)->toString();
	tiny_string name,ns;

	stringToQName(tmp,name,ns);

	LOG(LOG_CALLS,"Looking for definition of " << ns << " :: " << name);
	objAndLevel o=getGlobal()->getVariableByQName(name,ns);

	//assert(owner);
	//TODO: should raise an exception, for now just return undefined
	if(o.obj==NULL)
	{
		LOG(LOG_NOT_IMPLEMENTED,"NOT found");
		return new Undefined;
	}

	//Check if the object has to be defined
	if(o.obj->getObjectType()==T_DEFINABLE)
	{
		LOG(LOG_CALLS,"We got an object not yet valid");
		Definable* d=static_cast<Definable*>(o.obj);
		d->define(getGlobal());
		o=getGlobal()->getVariableByQName(name,ns);
	}

	assert(o.obj->getObjectType()==T_CLASS);

	LOG(LOG_CALLS,"Getting definition for " << ns << " :: " << name);
	return o.obj;
}

ASFUNCTIONBODY(lightspark,getTimer)
{
	uint64_t ret=compat_msectiming() - sys->startTime;
	return abstract_i(ret);
}

void Dictionary::sinit(Class_base* c)
{
	assert(c->constructor==NULL);
	c->constructor=new Function(_constructor);
}

void Dictionary::buildTraits(ASObject* o)
{
}

ASFUNCTIONBODY(Dictionary,_constructor)
{
}

bool Dictionary::setVariableByMultiname_i(const multiname& name, intptr_t value)
{
	return setVariableByMultiname(name,abstract_i(value));
}

bool Dictionary::setVariableByMultiname(const multiname& name, ASObject* o)
{
	assert(name.name_type==multiname::NAME_OBJECT);
	//We can use the [] operator, as the value is just a pointer and there is no side effect in creating one
	data[name.name_o]=o;
	//This is ugly, but at least we are sure that we own name_o
	multiname* tmp=const_cast<multiname*>(&name);
	tmp->name_o=NULL;
	return true;
}

bool Dictionary::deleteVariableByMultiname(const multiname& name)
{
	assert(name.name_type==multiname::NAME_OBJECT);
	map<ASObject*,ASObject*>::iterator it=data.find(name.name_o);
	assert(it!=data.end());

	ASObject* ret=it->second;
	ret->decRef();

	data.erase(it);

	//This is ugly, but at least we are sure that we own name_o
	multiname* tmp=const_cast<multiname*>(&name);
	tmp->name_o=NULL;
	return true;
}

bool Dictionary::getVariableByMultiname(const multiname& name, ASObject*& out)
{
	assert(name.name_type==multiname::NAME_OBJECT);
	map<ASObject*,ASObject*>::iterator it=data.find(name.name_o);
	if(it==data.end())
		out=new Undefined;
	else
	{
		ASObject* ret=it->second;
		ret->incRef();
		out=ret;
		//This is ugly, but at least we are sure that we own name_o
		multiname* tmp=const_cast<multiname*>(&name);
		tmp->name_o=NULL;
	}
	return true;
}

bool Dictionary::hasNext(int& index, bool& out)
{
	out=index<data.size();
	index++;
	return true;
}

bool Dictionary::nextName(int index, ASObject*& out)
{
	assert(index<data.size());
	map<ASObject*,ASObject*>::iterator it=data.begin();
	for(int i=0;i<index;i++)
		it++;
	out=it->first;
	return true;
}

bool Dictionary::nextValue(int index, ASObject*& out)
{
	abort();
/*	assert(index<data.size());
	map<ASObject*,ASObject*>::iterator it=data.begin();
	for(int i=0;i<index;i++)
		it++;
	out=it->first;
	return true;*/
}
