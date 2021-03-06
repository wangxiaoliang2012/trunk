// 2007,2008 © Václav Šmilauer <eudoxos@arcig.cz> 

#include<sstream>
#include<map>
#include<vector>
#include<unistd.h>
#include<list>
#include<signal.h>

#include<boost/python.hpp>
#include<boost/python/raw_function.hpp>
// unused now
#if 0
	#include<boost/python/suite/indexing/vector_indexing_suite.hpp>
#endif
#include<boost/bind.hpp>
#include<boost/lambda/bind.hpp>
#include<boost/thread/thread.hpp>
#include<boost/filesystem/operations.hpp>
#include<boost/date_time/posix_time/posix_time.hpp>
#include<boost/any.hpp>
#include<boost/python.hpp>
#include<boost/foreach.hpp>
#include<boost/algorithm/string.hpp>
#include<boost/version.hpp>

#include<yade/lib/base/Logging.hpp>
#include<yade/lib/pyutil/gil.hpp>
#include<yade/lib/pyutil/raw_constructor.hpp>
#include<yade/lib/pyutil/doc_opts.hpp>
#include<yade/core/Omega.hpp>
#include<yade/core/ThreadRunner.hpp>
#include<yade/core/FileGenerator.hpp>
#include<yade/core/EnergyTracker.hpp>

#include<yade/pkg/dem/STLImporter.hpp>

#include<yade/pkg/common/Dispatching.hpp>
#include<yade/core/GlobalEngine.hpp>
#include<yade/core/PartialEngine.hpp>
#include<yade/core/Functor.hpp>
#include<yade/pkg/common/ParallelEngine.hpp>
#include<yade/pkg/common/Collider.hpp>

#include<yade/pkg/common/InteractionLoop.hpp>

// #include<yade/pkg/dem/Shop.hpp>
#include<yade/core/Clump.hpp>

#if BOOST_VERSION>=104700
	#include<boost/math/special_functions/nonfinite_num_facets.hpp>
#else
	#include<boost/math/nonfinite_num_facets.hpp>
#endif

#include<locale>
#include<boost/archive/codecvt_null.hpp>

using namespace boost;
using namespace std;
namespace py = boost::python;

#include<yade/lib/serialization/ObjectIO.hpp>

#include<yade/extra/boost_python_len.hpp>

/*
Python normally iterates over object it is has __getitem__ and __len__, which BodyContainer does.
However, it will not skip removed bodies automatically, hence this iterator which does just that.
*/
class pyBodyIterator{
	BodyContainer::iterator I, Iend;
	public:
	pyBodyIterator(const shared_ptr<BodyContainer>& bc){ I=bc->begin(); Iend=bc->end(); }
	pyBodyIterator pyIter(){return *this;}
	shared_ptr<Body> pyNext(){
		BodyContainer::iterator ret;
		while(I!=Iend){ ret=I; ++I; if(*ret) return *ret; }
		PyErr_SetNone(PyExc_StopIteration); python::throw_error_already_set(); /* never reached, but makes the compiler happier */ throw;
	}
};

class pyBodyContainer{
	public:
	const shared_ptr<BodyContainer> proxee;
	pyBodyIterator pyIter(){return pyBodyIterator(proxee);}
	pyBodyContainer(const shared_ptr<BodyContainer>& _proxee): proxee(_proxee){}
	shared_ptr<Body> pyGetitem(Body::id_t _id){
		int id=(_id>=0 ? _id : proxee->size()+_id);
		if(id<0 || (size_t)id>=proxee->size()){ PyErr_SetString(PyExc_IndexError, "Body id out of range."); python::throw_error_already_set(); /* make compiler happy; never reached */ return shared_ptr<Body>(); }
		else return (*proxee)[id];
	}
	Body::id_t append(shared_ptr<Body> b){
		// shoud be >=0, but Body is by default created with id 0... :-|
		if(b->getId()>=0){ PyErr_SetString(PyExc_IndexError,("Body already has id "+lexical_cast<string>(b->getId())+" set; appending such body (for the second time) is not allowed.").c_str()); python::throw_error_already_set(); }
		return proxee->insert(b);
	}
	vector<Body::id_t> appendList(vector<shared_ptr<Body> > bb){
		/* prevent crash when adding lots of bodies (not clear why it happens exactly, bt is like this:

			#3  <signal handler called>
			#4  0x000000000052483f in boost::detail::atomic_increment (pw=0x8089) at /usr/include/boost/detail/sp_counted_base_gcc_x86.hpp:66
			#5  0x00000000005248b3 in boost::detail::sp_counted_base::add_ref_copy (this=0x8081) at /usr/include/boost/detail/sp_counted_base_gcc_x86.hpp:133
			#6  0x00000000005249ca in shared_count (this=0x7fff2e44db48, r=@0x7f08ffd692b8) at /usr/include/boost/detail/shared_count.hpp:227
			#7  0x00000000005258e3 in shared_ptr (this=0x7fff2e44db40) at /usr/include/boost/shared_ptr.hpp:165
			#8  0x0000000000505cff in BodyRedirectionVectorIterator::getValue (this=0x846f040) at /home/vaclav/yade/trunk/core/containers/BodyRedirectionVector.cpp:47
			#9  0x00007f0908af41ce in BodyContainerIteratorPointer::operator* (this=0x7fff2e44db60) at /home/vaclav/yade/build-trunk/include/yade-trunk/yade/core/BodyContainer.hpp:63
			#10 0x00007f0908af420a in boost::foreach_detail_::deref<BodyContainer, mpl_::bool_<false> > (cur=@0x7fff2e44db60) at /usr/include/boost/foreach.hpp:750
			#11 0x00007f0908adc5a9 in OpenGLRenderer::renderGeometricalModel (this=0x77f1240, scene=@0x1f49220) at pkg/common/RenderingEngine/OpenGLRenderer/OpenGLRenderer.cpp:441
			#12 0x00007f0908adfb84 in OpenGLRenderer::render (this=0x77f1240, scene=@0x1f49220, selection=-1) at pkg/common/RenderingEngine/OpenGLRenderer/OpenGLRenderer.cpp:232

		*/
		#if BOOST_VERSION<103500
			boost::try_mutex::scoped_try_lock lock(Omega::instance().renderMutex,true); // acquire lock on the mutex (true)
		#else
			boost::mutex::scoped_lock lock(Omega::instance().renderMutex);
		#endif
		vector<Body::id_t> ret; FOREACH(shared_ptr<Body>& b, bb){ret.push_back(append(b));} return ret;
	}
	Body::id_t clump(vector<Body::id_t> ids){
		// create and add clump itself
		Scene* scene(Omega::instance().getScene().get());
		shared_ptr<Body> clumpBody=shared_ptr<Body>(new Body());
		shared_ptr<Clump> clump=shared_ptr<Clump>(new Clump());
		clumpBody->shape=clump;
		clumpBody->setBounded(false);
		proxee->insert(clumpBody);
		// add clump members to the clump
		FOREACH(Body::id_t id, ids) {
			if (Body::byId(id,scene)->isClumpMember()){	//Check, whether the body is clumpMember
				Clump::del(Body::byId(Body::byId(id,scene)->clumpId,scene),Body::byId(id,scene)); //If so, remove it from there
			}
		};
		
		FOREACH(Body::id_t id, ids) Clump::add(clumpBody,Body::byId(id,scene));
		Clump::updateProperties(clumpBody,/*intersecting*/ false);
		return clumpBody->getId();
	}
	python::tuple appendClump(vector<shared_ptr<Body> > bb){
		// append constituent particles
		vector<Body::id_t> ids(appendList(bb));
		// clump them together (the clump fcn) and return
		return python::make_tuple(clump(ids),ids);
	}
	vector<Body::id_t> replace(vector<shared_ptr<Body> > bb){proxee->clear(); return appendList(bb);}
	long length(){return proxee->size();}
	void clear(){proxee->clear();}
	bool erase(Body::id_t id){ return proxee->erase(id); }
};


class pyTags{
	public:
		pyTags(const shared_ptr<Scene> _mb): mb(_mb){}
		const shared_ptr<Scene> mb;
		bool hasKey(const string& key){ FOREACH(string val, mb->tags){ if(algorithm::starts_with(val,key+"=")){ return true;} } return false; }
		string getItem(const string& key){
			FOREACH(string& val, mb->tags){
				if(algorithm::starts_with(val,key+"=")){ string val1(val); algorithm::erase_head(val1,key.size()+1); return val1;}
			}
			PyErr_SetString(PyExc_KeyError,("Invalid key: "+key+".").c_str());
			python::throw_error_already_set(); /* make compiler happy; never reached */ return string();
		}
		void setItem(const string& key,const string& item){
			if(key.find("=")!=string::npos) {
				PyErr_SetString(PyExc_KeyError, "Key must not contain the '=' character (implementation limitation; sorry).");
				python::throw_error_already_set();
			}
			FOREACH(string& val, mb->tags){if(algorithm::starts_with(val,key+"=")){ val=key+"="+item; return; } }
			mb->tags.push_back(key+"="+item);
			}
		python::list keys(){
			python::list ret;
			FOREACH(string val, mb->tags){
				size_t i=val.find("=");
				if(i==string::npos) throw runtime_error("Tags must be in the key=value format (internal error?)");
				algorithm::erase_tail(val,val.size()-i); ret.append(val);
			}
			return ret;
		}
};


class pyInteractionIterator{
	InteractionContainer::iterator I, Iend;
	public:
	pyInteractionIterator(const shared_ptr<InteractionContainer>& ic){ I=ic->begin(); Iend=ic->end(); }
	pyInteractionIterator pyIter(){return *this;}
	shared_ptr<Interaction> pyNext(){
		InteractionContainer::iterator ret;
		while(I!=Iend){ ret=I; ++I; if((*ret)->isReal()) return *ret; }
		PyErr_SetNone(PyExc_StopIteration); python::throw_error_already_set();
		throw; // to avoid compiler warning; never reached
		//InteractionContainer::iterator ret=I; ++I; return *ret;
	}
};

class pyInteractionContainer{
	public:
		const shared_ptr<InteractionContainer> proxee;
		pyInteractionContainer(const shared_ptr<InteractionContainer>& _proxee): proxee(_proxee){}
		pyInteractionIterator pyIter(){return pyInteractionIterator(proxee);}
		shared_ptr<Interaction> pyGetitem(vector<Body::id_t> id12){
			//if(!PySequence_Check(id12.ptr())) throw invalid_argument("Key must be a tuple");
			//if(python::len(id12)!=2) throw invalid_argument("Key must be a 2-tuple: id1,id2.");
			if(id12.size()==2){
				//if(max(id12[0],id12[1])>
				shared_ptr<Interaction> i=proxee->find(id12[0],id12[1]);
				if(i) return i; else { PyErr_SetString(PyExc_IndexError,"No such interaction"); python::throw_error_already_set(); /* make compiler happy; never reached */ return shared_ptr<Interaction>(); }
			}
			else if(id12.size()==1){ return (*proxee)[id12[0]];}
			else throw invalid_argument("2 integers (id1,id2) or 1 integer (nth) required.");
		}
		/* return nth _real_ iteration from the container (0-based index); this is to facilitate picking random interaction */
		shared_ptr<Interaction> pyNth(long n){
			long i=0; FOREACH(shared_ptr<Interaction> I, *proxee){ if(!I->isReal()) continue; if(i++==n) return I; }
			PyErr_SetString(PyExc_IndexError,(string("Interaction number out of range (")+lexical_cast<string>(n)+">="+lexical_cast<string>(i)+").").c_str());
			python::throw_error_already_set(); /* make compiler happy; never reached */ return shared_ptr<Interaction>();
		}
		long len(){return proxee->size();}
		void clear(){proxee->clear();}
		python::list withBody(long id){ python::list ret; FOREACH(const shared_ptr<Interaction>& I, *proxee){ if(I->isReal() && (I->getId1()==id || I->getId2()==id)) ret.append(I);} return ret;}
		python::list withBodyAll(long id){ python::list ret; FOREACH(const shared_ptr<Interaction>& I, *proxee){ if(I->getId1()==id || I->getId2()==id) ret.append(I);} return ret; }
		long countReal(){ long ret=0; FOREACH(const shared_ptr<Interaction>& I, *proxee){ if(I->isReal()) ret++; } return ret; }
		bool serializeSorted_get(){return proxee->serializeSorted;}
		void serializeSorted_set(bool ss){proxee->serializeSorted=ss;}
		void eraseNonReal(){ proxee->eraseNonReal(); }
		void erase(Body::id_t id1, Body::id_t id2){ proxee->requestErase(id1,id2); }
};

class pyForceContainer{
		shared_ptr<Scene> scene;
	public:
		pyForceContainer(shared_ptr<Scene> _scene): scene(_scene) { }
		void checkId(long id){ if(id<0 || (size_t)id>=scene->bodies->size()){ PyErr_SetString(PyExc_IndexError, "Body id out of range."); python::throw_error_already_set(); /* never reached */ throw; } }
		Vector3r force_get(long id){  checkId(id); return scene->forces.getForceSingle(id); /* scene->forces.sync(); return scene->forces.getForce(id); */}
		Vector3r torque_get(long id){ checkId(id); return scene->forces.getTorqueSingle(id); }
		Vector3r move_get(long id){   checkId(id); return scene->forces.getMoveSingle(id); }
		Vector3r rot_get(long id){    checkId(id); return scene->forces.getRotSingle(id); }
		void force_add(long id, const Vector3r& f){  checkId(id); scene->forces.addForce (id,f); }
		void torque_add(long id, const Vector3r& t){ checkId(id); scene->forces.addTorque(id,t);}
		void move_add(long id, const Vector3r& t){   checkId(id); scene->forces.addMove(id,t);}
		void rot_add(long id, const Vector3r& t){    checkId(id); scene->forces.addRot(id,t);}
		long syncCount_get(){ return scene->forces.syncCount;}
		void syncCount_set(long count){ scene->forces.syncCount=count;}
};

class pyMaterialContainer{
		shared_ptr<Scene> scene;
	public:
		pyMaterialContainer(shared_ptr<Scene> _scene): scene(_scene) { }
		shared_ptr<Material> getitem_id(int _id){ int id=(_id>=0 ? _id : scene->materials.size()+_id); if(id<0 || (size_t)id>=scene->materials.size()){ PyErr_SetString(PyExc_IndexError, "Material id out of range."); python::throw_error_already_set(); /* never reached */ throw; } return Material::byId(id,scene); }
		shared_ptr<Material> getitem_label(string label){
			// translate runtime_error to KeyError (instead of RuntimeError) if the material doesn't exist
			try { return Material::byLabel(label,scene);	}
			catch (std::runtime_error& e){ PyErr_SetString(PyExc_KeyError,e.what()); python::throw_error_already_set(); /* never reached; avoids warning */ throw; }
		}
		int append(shared_ptr<Material> m){ scene->materials.push_back(m); m->id=scene->materials.size()-1; return m->id; }
		vector<int> appendList(vector<shared_ptr<Material> > mm){ vector<int> ret; FOREACH(shared_ptr<Material>& m, mm) ret.push_back(append(m)); return ret; }
		int len(){ return (int)scene->materials.size(); }
		int index(const std::string& label){ return Material::byLabelIndex(label,scene.get()); }
};

void termHandlerNormal(int sig){cerr<<"Yade: normal exit."<<endl; raise(SIGTERM);}
void termHandlerError(int sig){cerr<<"Yade: error exit."<<endl; raise(SIGTERM);}

class pyOmega{
	private:
		// can be safely removed now, since pyOmega makes an empty scene in the constructor, if there is none
		void assertScene(){if(!OMEGA.getScene()) throw std::runtime_error("No Scene instance?!"); }
		Omega& OMEGA;
	public:
	pyOmega(): OMEGA(Omega::instance()){
		shared_ptr<Scene> rb=OMEGA.getScene();
		if(!rb){
			OMEGA.init();
			rb=OMEGA.getScene();
		}
		assert(rb);
		if(!OMEGA.hasSimulationLoop()){
			OMEGA.createSimulationLoop();
		}
	};
	/* Create variables in python's __builtin__ namespace that correspond to labeled objects. At this moment, only engines and functors can be labeled (not bodies etc). */
	void mapLabeledEntitiesToVariables(){
		// not sure if we should map materials to variables by default...
		// a call to this functions would have to be added to pyMaterialContainer::append
		#if 0
			FOREACH(const shared_ptr<Material>& m, OMEGA.getScene()->materials){
				if(!m->label.empty()) { PyGILState_STATE gstate; gstate = PyGILState_Ensure(); PyRun_SimpleString(("__builtins__."+m->label+"=Omega().materials["+lexical_cast<string>(m->id)+"]").c_str()); PyGILState_Release(gstate); }
			}
		#endif
		FOREACH(const shared_ptr<Engine>& e, OMEGA.getScene()->engines){
			if(!e->label.empty()){
				pyRunString("__builtins__."+e->label+"=Omega().labeledEngine('"+e->label+"')");
			}
			#define _DO_FUNCTORS(functors,FunctorT){ FOREACH(const shared_ptr<FunctorT>& f, functors){ if(!f->label.empty()){ pyRunString("__builtins__."+f->label+"=Omega().labeledEngine('"+f->label+"')");}} }
			#define _TRY_DISPATCHER(DispatcherT) { DispatcherT* d=dynamic_cast<DispatcherT*>(e.get()); if(d){ _DO_FUNCTORS(d->functors,DispatcherT::FunctorType); } }
			_TRY_DISPATCHER(BoundDispatcher); _TRY_DISPATCHER(IGeomDispatcher); _TRY_DISPATCHER(IPhysDispatcher); _TRY_DISPATCHER(LawDispatcher);
			InteractionLoop* id=dynamic_cast<InteractionLoop*>(e.get());
			if(id){
				_DO_FUNCTORS(id->geomDispatcher->functors,IGeomFunctor);
				_DO_FUNCTORS(id->physDispatcher->functors,IPhysFunctor);
				_DO_FUNCTORS(id->lawDispatcher->functors,LawFunctor);
			}
			Collider* coll=dynamic_cast<Collider*>(e.get());
			if(coll){ _DO_FUNCTORS(coll->boundDispatcher->functors,BoundFunctor); }
			#undef _DO_FUNCTORS
			#undef _TRY_DISPATCHER
		}
	}
	python::object labeled_engine_get(string label){
		FOREACH(const shared_ptr<Engine>& e, OMEGA.getScene()->engines){
			#define _DO_FUNCTORS(functors,FunctorT){ FOREACH(const shared_ptr<FunctorT>& f, functors){ if(f->label==label) return python::object(f); }}
			#define _TRY_DISPATCHER(DispatcherT) { DispatcherT* d=dynamic_cast<DispatcherT*>(e.get()); if(d){ _DO_FUNCTORS(d->functors,DispatcherT::FunctorType); } }
			if(e->label==label){ return python::object(e); }
			_TRY_DISPATCHER(BoundDispatcher); _TRY_DISPATCHER(IGeomDispatcher); _TRY_DISPATCHER(IPhysDispatcher); _TRY_DISPATCHER(LawDispatcher);
			InteractionLoop* id=dynamic_cast<InteractionLoop*>(e.get());
			if(id){
				_DO_FUNCTORS(id->geomDispatcher->functors,IGeomFunctor);
				_DO_FUNCTORS(id->physDispatcher->functors,IPhysFunctor);
				_DO_FUNCTORS(id->lawDispatcher->functors,LawFunctor);
			}
			Collider* coll=dynamic_cast<Collider*>(e.get());
			if(coll){ _DO_FUNCTORS(coll->boundDispatcher->functors,BoundFunctor); }
			#undef _DO_FUNCTORS
			#undef _TRY_DISPATCHER
		}
		throw std::invalid_argument(string("No engine labeled `")+label+"'");
	}

	long iter(){ return OMEGA.getScene()->iter;}
	int subStep(){ return OMEGA.getScene()->subStep; }
	bool subStepping_get(){ return OMEGA.getScene()->subStepping; }
	void subStepping_set(bool val){ OMEGA.getScene()->subStepping=val; }

	double time(){return OMEGA.getScene()->time;}
	double realTime(){ return OMEGA.getRealTime(); }
	double speed(){ return OMEGA.getScene()->speed; }
	double dt_get(){return OMEGA.getScene()->dt;}
	void dt_set(double dt){
		Scene* scene=OMEGA.getScene().get();
		// activate timestepper, if possible (throw exception if there is none)
		if(dt<0){ if(!scene->timeStepperActivate(true)) /* not activated*/ throw runtime_error("No TimeStepper found in O.engines."); }
		else { scene->dt=dt; }
	}
	bool dynDt_get(){return OMEGA.getScene()->timeStepperActive();}
	bool dynDt_set(bool activate){if(!OMEGA.getScene()->timeStepperActivate(activate)) /* not activated*/ throw runtime_error("No TimeStepper found in O.engines."); return true;}
	bool dynDtAvailable_get(){ return OMEGA.getScene()->timeStepperPresent(); }
	long stopAtIter_get(){return OMEGA.getScene()->stopAtIter; }
	void stopAtIter_set(long s){OMEGA.getScene()->stopAtIter=s; }


	bool timingEnabled_get(){return TimingInfo::enabled;}
	void timingEnabled_set(bool enabled){TimingInfo::enabled=enabled;}
	// deprecated:
		unsigned long forceSyncCount_get(){ return OMEGA.getScene()->forces.syncCount;}
		void forceSyncCount_set(unsigned long count){ OMEGA.getScene()->forces.syncCount=count;}

	void run(long int numIter=-1,bool doWait=false){
		Scene* scene=OMEGA.getScene().get();
		if(numIter>0) scene->stopAtIter=scene->iter+numIter;
		OMEGA.run();
		// timespec t1,t2; t1.tv_sec=0; t1.tv_nsec=40000000; /* 40 ms */
		// while(!OMEGA.isRunning()) nanosleep(&t1,&t2); // wait till we start, so that calling wait() immediately afterwards doesn't return immediately
		LOG_DEBUG("RUN"<<((scene->stopAtIter-scene->iter)>0?string(" ("+lexical_cast<string>(scene->stopAtIter-scene->iter)+" to go)"):string(""))<<"!");
		if(doWait) wait();
	}
	void pause(){Py_BEGIN_ALLOW_THREADS; OMEGA.pause(); Py_END_ALLOW_THREADS; LOG_DEBUG("PAUSE!");}
	void step() { if(OMEGA.isRunning()) throw runtime_error("Called O.step() while simulation is running."); OMEGA.getScene()->moveToNextTimeStep(); /* LOG_DEBUG("STEP!"); run(1); wait(); */ }
	void wait(){
		if(OMEGA.isRunning()){LOG_DEBUG("WAIT!");} else return;
		timespec t1,t2; t1.tv_sec=0; t1.tv_nsec=40000000; /* 40 ms */ Py_BEGIN_ALLOW_THREADS; while(OMEGA.isRunning()) nanosleep(&t1,&t2); Py_END_ALLOW_THREADS;
		if(!OMEGA.simulationLoop->workerThrew) return;
		LOG_ERROR("Simulation error encountered."); OMEGA.simulationLoop->workerThrew=false; throw OMEGA.simulationLoop->workerException;
	}
	bool isRunning(){ return OMEGA.isRunning(); }
	python::object get_filename(){ string f=OMEGA.sceneFile; if(f.size()>0) return python::object(f); return python::object();}
	void load(std::string fileName,bool quiet=false) {
		Py_BEGIN_ALLOW_THREADS; OMEGA.stop(); Py_END_ALLOW_THREADS; 
		OMEGA.loadSimulation(fileName,quiet);
		OMEGA.createSimulationLoop();
		mapLabeledEntitiesToVariables();
	}
	void reload(bool quiet=false){	load(OMEGA.sceneFile,quiet);}
	void saveTmp(string mark="", bool quiet=false){ save(":memory:"+mark,quiet);}
	void loadTmp(string mark="", bool quiet=false){ load(":memory:"+mark,quiet);}
	python::list lsTmp(){ python::list ret; typedef pair<std::string,string> strstr; FOREACH(const strstr& sim,OMEGA.memSavedSimulations){ string mark=sim.first; boost::algorithm::replace_first(mark,":memory:",""); ret.append(mark); } return ret; }
	void tmpToFile(string mark, string filename){
		if(OMEGA.memSavedSimulations.count(":memory:"+mark)==0) throw runtime_error("No memory-saved simulation named "+mark);
		iostreams::filtering_ostream out;
		if(algorithm::ends_with(filename,".bz2")) out.push(iostreams::bzip2_compressor());
		out.push(iostreams::file_sink(filename));
		if(!out.good()) throw runtime_error("Error while opening file `"+filename+"' for writing.");
		LOG_INFO("Saving :memory:"<<mark<<" to "<<filename);
		out<<OMEGA.memSavedSimulations[":memory:"+mark];
	}
	string tmpToString(string mark){
		if(OMEGA.memSavedSimulations.count(":memory:"+mark)==0) throw runtime_error("No memory-saved simulation named "+mark);
		return OMEGA.memSavedSimulations[":memory:"+mark];
	}

	void reset(){Py_BEGIN_ALLOW_THREADS; OMEGA.reset(); Py_END_ALLOW_THREADS; }
	void resetThisScene(){Py_BEGIN_ALLOW_THREADS; OMEGA.stop(); Py_END_ALLOW_THREADS; OMEGA.resetCurrentScene(); OMEGA.createSimulationLoop();}
	void resetCurrentScene(){Py_BEGIN_ALLOW_THREADS; OMEGA.stop(); Py_END_ALLOW_THREADS; OMEGA.resetCurrentScene(); OMEGA.createSimulationLoop();}
	void resetTime(){ OMEGA.getScene()->iter=0; OMEGA.getScene()->time=0; OMEGA.timeInit(); }
	void switchScene(){ std::swap(OMEGA.scenes[OMEGA.currentSceneNb],OMEGA.sceneAnother); }
	void resetAllScenes(){Py_BEGIN_ALLOW_THREADS; OMEGA.stop(); Py_END_ALLOW_THREADS; OMEGA.resetAllScenes(); OMEGA.createSimulationLoop();}
	shared_ptr<Scene> scene_get(){ return OMEGA.getScene(); }
	int addScene(){return OMEGA.addScene();}
	void switchToScene(int i){OMEGA.switchToScene(i);}

	void save(std::string fileName,bool quiet=false){
		assertScene();
		OMEGA.saveSimulation(fileName,quiet);
		// OMEGA.sceneFile=fileName; // done in Omega::saveSimulation;
	}
	
	python::list miscParams_get(){
		python::list ret;
		FOREACH(shared_ptr<Serializable>& s, OMEGA.getScene()->miscParams){
			ret.append(s);
		}
		return ret;
	}

	void miscParams_set(vector<shared_ptr<Serializable> > ss){
		vector<shared_ptr<Serializable> >& miscParams=OMEGA.getScene()->miscParams;
		miscParams.clear();
		FOREACH(shared_ptr<Serializable> s, ss){
			miscParams.push_back(s);
		}
	}


	vector<shared_ptr<Engine> > engines_get(void){assertScene(); Scene* scene=OMEGA.getScene().get(); return scene->_nextEngines.empty()?scene->engines:scene->_nextEngines;}
	void engines_set(const vector<shared_ptr<Engine> >& egs){
		assertScene(); Scene* scene=OMEGA.getScene().get();
		if(scene->subStep<0) scene->engines=egs; // not inside the engine loop right now, ok to update directly
		else scene->_nextEngines=egs; // inside the engine loop, update _nextEngines; O.engines picks that up automatically, and Scene::moveToNextTimestep will put them in place of engines at the start of the next loop
		mapLabeledEntitiesToVariables();
	}
	// raw access to engines/_nextEngines, for debugging
	vector<shared_ptr<Engine> > currEngines_get(){ return OMEGA.getScene()->engines; }
	vector<shared_ptr<Engine> > nextEngines_get(){ return OMEGA.getScene()->_nextEngines; }

	pyBodyContainer bodies_get(void){assertScene(); return pyBodyContainer(OMEGA.getScene()->bodies); }
	pyInteractionContainer interactions_get(void){assertScene(); return pyInteractionContainer(OMEGA.getScene()->interactions); }
	
	pyForceContainer forces_get(void){return pyForceContainer(OMEGA.getScene());}
	pyMaterialContainer materials_get(void){return pyMaterialContainer(OMEGA.getScene());}
	

	python::list listChildClassesNonrecursive(const string& base){
		python::list ret;
		for(map<string,DynlibDescriptor>::const_iterator di=Omega::instance().getDynlibsDescriptor().begin();di!=Omega::instance().getDynlibsDescriptor().end();++di) if (Omega::instance().isInheritingFrom((*di).first,base)) ret.append(di->first);
		return ret;
	}

	bool isChildClassOf(const string& child, const string& base){
		return (Omega::instance().isInheritingFrom_recursive(child,base));
	}

	python::list plugins_get(){
		const map<string,DynlibDescriptor>& plugins=Omega::instance().getDynlibsDescriptor();
		std::pair<string,DynlibDescriptor> p; python::list ret;
		FOREACH(p, plugins) ret.append(p.first);
		return ret;
	}

	pyTags tags_get(void){assertScene(); return pyTags(OMEGA.getScene());}

	void interactionContainer_set(string clss){
		Scene* rb=OMEGA.getScene().get();
		if(rb->interactions->size()>0) throw std::runtime_error("Interaction container not empty, will not change its class.");
		shared_ptr<InteractionContainer> ic=dynamic_pointer_cast<InteractionContainer>(ClassFactory::instance().createShared(clss));
		rb->interactions=ic;
	}
	string interactionContainer_get(string clss){ return OMEGA.getScene()->interactions->getClassName(); }

	void bodyContainer_set(string clss){
		Scene* rb=OMEGA.getScene().get();
		if(rb->bodies->size()>0) throw std::runtime_error("Body container not empty, will not change its class.");
		shared_ptr<BodyContainer> bc=dynamic_pointer_cast<BodyContainer>(ClassFactory::instance().createShared(clss));
		rb->bodies=bc;
	}
	string bodyContainer_get(string clss){ return OMEGA.getScene()->bodies->getClassName(); }
	#ifdef YADE_OPENMP
		int numThreads_get(){ return omp_get_max_threads();}
		void numThreads_set(int n){ int bcn=OMEGA.getScene()->forces.getNumAllocatedThreads(); if(bcn<n) LOG_WARN("ForceContainer has only "<<bcn<<" threads allocated. Changing thread number to on "<<bcn<<" instead of "<<n<<" requested."); omp_set_num_threads(min(n,bcn)); LOG_WARN("BUG: Omega().numThreads=n doesn't work as expected (number of threads is not changed globally). Set env var OMP_NUM_THREADS instead."); }
	#else
		int numThreads_get(){return 1;}
		void numThreads_set(int n){ LOG_WARN("Yade was compiled without openMP support, changing number of threads will have no effect."); }
	#endif
	
	shared_ptr<Cell> cell_get(){ if(OMEGA.getScene()->isPeriodic) return OMEGA.getScene()->cell; return shared_ptr<Cell>(); }
	bool periodic_get(void){ return OMEGA.getScene()->isPeriodic; } 
	void periodic_set(bool v){ OMEGA.getScene()->isPeriodic=v; }

	shared_ptr<EnergyTracker> energy_get(){ return OMEGA.getScene()->energy; }
	bool trackEnergy_get(void){ return OMEGA.getScene()->trackEnergy; }
	void trackEnergy_set(bool e){ OMEGA.getScene()->trackEnergy=e; }

	void disableGdb(){
		signal(SIGSEGV,SIG_DFL);
		signal(SIGABRT,SIG_DFL);
	}
	void exitNoBacktrace(int status=0){
		if(status==0) signal(SIGSEGV,termHandlerNormal); /* unset the handler that runs gdb and prints backtrace */
		else signal(SIGSEGV,termHandlerError);
		// try to clean our mess
		Omega::instance().cleanupTemps();
		// flush all streams (so that in case we crash at exit, unflushed buffers are not lost)
		fflush(NULL);
		// attempt exit
		exit(status);
	}
	void runEngine(const shared_ptr<Engine>& e){ LOG_WARN("Omega().runEngine(): deprecated, use __call__ method of the engine instance directly instead; will be removed in the future."); e->scene=OMEGA.getScene().get(); e->action(); }
	std::string tmpFilename(){ return OMEGA.tmpFilename(); }
};

BOOST_PYTHON_MODULE(wrapper)
{
	python::scope().attr("__doc__")="Wrapper for c++ internals of yade.";

	YADE_SET_DOCSTRING_OPTS;

	python::enum_<yade::Attr::flags>("AttrFlags")
		.value("noSave",yade::Attr::noSave)
		.value("readonly",yade::Attr::readonly)
		.value("triggerPostLoad",yade::Attr::triggerPostLoad)
		.value("noResize",yade::Attr::noResize)
    ;

	python::class_<pyOmega>("Omega")
		.add_property("iter",&pyOmega::iter,"Get current step number")
		.add_property("subStep",&pyOmega::subStep,"Get the current subStep number (only meaningful if O.subStepping==True); -1 when outside the loop, otherwise either 0 (O.subStepping==False) or number of engine to be run (O.subStepping==True)")
		.add_property("subStepping",&pyOmega::subStepping_get,&pyOmega::subStepping_set,"Get/set whether subStepping is active.")
		.add_property("stopAtIter",&pyOmega::stopAtIter_get,&pyOmega::stopAtIter_set,"Get/set number of iteration after which the simulation will stop.")
		.add_property("time",&pyOmega::time,"Return virtual (model world) time of the simulation.")
		.add_property("realtime",&pyOmega::realTime,"Return clock (human world) time the simulation has been running.")
		.add_property("speed",&pyOmega::speed,"Return current calculation speed [iter/sec].")
		.add_property("dt",&pyOmega::dt_get,&pyOmega::dt_set,"Current timestep (Δt) value.")
		.add_property("dynDt",&pyOmega::dynDt_get,&pyOmega::dynDt_set,"Whether a :yref:`TimeStepper` is used for dynamic Δt control. See :yref:`dt<Omega.dt>` on how to enable/disable :yref:`TimeStepper`.")
		.add_property("dynDtAvailable",&pyOmega::dynDtAvailable_get,"Whether a :yref:`TimeStepper` is amongst :yref:`O.engines<Omega.engines>`, activated or not.")
		.def("load",&pyOmega::load,(py::arg("file"),py::arg("quiet")=false),"Load simulation from file.")
		.def("reload",&pyOmega::reload,(py::arg("quiet")=false),"Reload current simulation")
		.def("save",&pyOmega::save,(py::arg("file"),py::arg("quiet")=false),"Save current simulation to file (should be .xml or .xml.bz2)")
		.def("loadTmp",&pyOmega::loadTmp,(py::arg("mark")="",py::arg("quiet")=false),"Load simulation previously stored in memory by saveTmp. *mark* optionally distinguishes multiple saved simulations")
		.def("saveTmp",&pyOmega::saveTmp,(py::arg("mark")="",py::arg("quiet")=false),"Save simulation to memory (disappears at shutdown), can be loaded later with loadTmp. *mark* optionally distinguishes different memory-saved simulations.")
		.def("lsTmp",&pyOmega::lsTmp,"Return list of all memory-saved simulations.")
		.def("tmpToFile",&pyOmega::tmpToFile,(python::arg("fileName"),python::arg("mark")=""),"Save XML of :yref:`saveTmp<Omega.saveTmp>`'d simulation into *fileName*.")
		.def("tmpToString",&pyOmega::tmpToString,(python::arg("mark")=""),"Return XML of :yref:`saveTmp<Omega.saveTmp>`'d simulation as string.")
		.def("run",&pyOmega::run,(python::arg("nSteps")=-1,python::arg("wait")=false),"Run the simulation. *nSteps* how many steps to run, then stop (if positive); *wait* will cause not returning to python until simulation will have stopped.")
		.def("pause",&pyOmega::pause,"Stop simulation execution. (May be called from within the loop, and it will stop after the current step).")
		.def("step",&pyOmega::step,"Advance the simulation by one step. Returns after the step will have finished.")
		.def("wait",&pyOmega::wait,"Don't return until the simulation will have been paused. (Returns immediately if not running).")
		.add_property("running",&pyOmega::isRunning,"Whether background thread is currently running a simulation.")
		.add_property("filename",&pyOmega::get_filename,"Filename under which the current simulation was saved (None if never saved).")
		.def("reset",&pyOmega::reset,"Reset simulations completely (including another scenes!).")
		.def("resetThisScene",&pyOmega::resetThisScene,"Reset current scene.")
		.def("resetCurrentScene",&pyOmega::resetCurrentScene,"Reset current scene.")
		.def("resetAllScenes",&pyOmega::resetAllScenes,"Reset all scenes.")
		.def("addScene",&pyOmega::addScene,"Add new scene to Omega, returns its number")
		.def("switchToScene",&pyOmega::switchToScene,"Switch to defined scene. Default scene has number 0, other scenes have to be created by addScene method.")
		.def("switchScene",&pyOmega::switchScene,"Switch to alternative simulation (while keeping the old one). Calling the function again switches back to the first one. Note that most variables from the first simulation will still refer to the first simulation even after the switch\n(e.g. b=O.bodies[4]; O.switchScene(); [b still refers to the body in the first simulation here])")
		.def("labeledEngine",&pyOmega::labeled_engine_get,"Return instance of engine/functor with the given label. This function shouldn't be called by the user directly; every ehange in O.engines will assign respective global python variables according to labels.\n\nFor example::\n\tO.engines=[InsertionSortCollider(label='collider')]\n\tcollider.nBins=5 ## collider has become a variable after assignment to O.engines automatically)")
		.def("resetTime",&pyOmega::resetTime,"Reset simulation time: step number, virtual and real time. (Doesn't touch anything else, including timings).")
		.def("plugins",&pyOmega::plugins_get,"Return list of all plugins registered in the class factory.")
		.def("_sceneObj",&pyOmega::scene_get,"Return the :yref:`scene <Scene>` object. Debugging only, all (or most) :yref:`Scene` functionality is proxies through :yref:`Omega`.")
		.add_property("engines",&pyOmega::engines_get,&pyOmega::engines_set,"List of engines in the simulation (Scene::engines).")
		.add_property("_currEngines",&pyOmega::currEngines_get,"Currently running engines; debugging only!")
		.add_property("_nextEngines",&pyOmega::nextEngines_get,"Engines for the next step, if different from the current ones, otherwise empty; debugging only!")
		.add_property("miscParams",&pyOmega::miscParams_get,&pyOmega::miscParams_set,"MiscParams in the simulation (Scene::mistParams), usually used to save serializables that don't fit anywhere else, like GL functors")
		.add_property("bodies",&pyOmega::bodies_get,"Bodies in the current simulation (container supporting index access by id and iteration)")
		.add_property("interactions",&pyOmega::interactions_get,"Interactions in the current simulation (container supporting index acces by either (id1,id2) or interactionNumber and iteration)")
		.add_property("materials",&pyOmega::materials_get,"Shared materials; they can be accessed by id or by label")
		.add_property("forces",&pyOmega::forces_get,":yref:`ForceContainer` (forces, torques, displacements) in the current simulation.")
		.add_property("energy",&pyOmega::energy_get,":yref:`EnergyTracker` of the current simulation. (meaningful only with :yref:`O.trackEnergy<Omega.trackEnergy>`)")
		.add_property("trackEnergy",&pyOmega::trackEnergy_get,&pyOmega::trackEnergy_set,"When energy tracking is enabled or disabled in this simulation.")
		.add_property("tags",&pyOmega::tags_get,"Tags (string=string dictionary) of the current simulation (container supporting string-index access/assignment)")
		.def("childClassesNonrecursive",&pyOmega::listChildClassesNonrecursive,"Return list of all classes deriving from given class, as registered in the class factory")
		.def("isChildClassOf",&pyOmega::isChildClassOf,"Tells whether the first class derives from the second one (both given as strings).")
		.add_property("timingEnabled",&pyOmega::timingEnabled_get,&pyOmega::timingEnabled_set,"Globally enable/disable timing services (see documentation of the :yref:`timing module<yade.timing>`).")
		.add_property("forceSyncCount",&pyOmega::forceSyncCount_get,&pyOmega::forceSyncCount_set,"Counter for number of syncs in ForceContainer, for profiling purposes.")
		.add_property("numThreads",&pyOmega::numThreads_get /* ,&pyOmega::numThreads_set*/ ,"Get maximum number of threads openMP can use.")
		.add_property("cell",&pyOmega::cell_get,"Periodic cell of the current scene (None if the scene is aperiodic).")
		.add_property("periodic",&pyOmega::periodic_get,&pyOmega::periodic_set,"Get/set whether the scene is periodic or not (True/False).")
		.def("exitNoBacktrace",&pyOmega::exitNoBacktrace,(python::arg("status")=0),"Disable SEGV handler and exit, optionally with given status number.")
		.def("disableGdb",&pyOmega::disableGdb,"Revert SEGV and ABRT handlers to system defaults.")
		.def("runEngine",&pyOmega::runEngine,"Run given engine exactly once; simulation time, step number etc. will not be incremented (use only if you know what you do).")
		.def("tmpFilename",&pyOmega::tmpFilename,"Return unique name of file in temporary directory which will be deleted when yade exits.")
		;
	python::class_<pyTags>("TagsWrapper","Container emulating dictionary semantics for accessing tags associated with simulation. Tags are accesed by strings.",python::init<pyTags&>())
		.def("__getitem__",&pyTags::getItem)
		.def("__setitem__",&pyTags::setItem)
		.def("keys",&pyTags::keys)
		.def("has_key",&pyTags::hasKey);
	python::class_<pyBodyContainer>("BodyContainer",python::init<pyBodyContainer&>())
		.def("__getitem__",&pyBodyContainer::pyGetitem)
		.def("__len__",&pyBodyContainer::length)
		.def("__iter__",&pyBodyContainer::pyIter)
		.def("append",&pyBodyContainer::append,"Append one Body instance, return its id.")
		.def("append",&pyBodyContainer::appendList,"Append list of Body instance, return list of ids")
		.def("appendClumped",&pyBodyContainer::appendClump,"Append given list of bodies as a clump (rigid aggregate); return list of ids.")
		.def("clump",&pyBodyContainer::clump,"Clump given bodies together (creating a rigid aggregate); returns clump id.")
		.def("clear", &pyBodyContainer::clear,"Remove all bodies (interactions not checked)")
		.def("erase", &pyBodyContainer::erase,"Erase body with the given id; all interaction will be deleted by InteractionLoop in the next step.")
		.def("replace",&pyBodyContainer::replace);
	python::class_<pyBodyIterator>("BodyIterator",python::init<pyBodyIterator&>())
		.def("__iter__",&pyBodyIterator::pyIter)
		.def("next",&pyBodyIterator::pyNext);
	python::class_<pyInteractionContainer>("InteractionContainer","Access to :yref:`interactions<Interaction>` of simulation, by using \n\n#. id's of both :yref:`Bodies<Body>` of the interactions, e.g. ``O.interactions[23,65]``\n#. iteraction over the whole container::\n\n\tfor i in O.interactions: print i.id1,i.id2\n\n.. note::\n\tIteration silently skips interactions that are not :yref:`real<Interaction.isReal>`.",python::init<pyInteractionContainer&>())
		.def("__iter__",&pyInteractionContainer::pyIter)
		.def("__getitem__",&pyInteractionContainer::pyGetitem)
		.def("__len__",&pyInteractionContainer::len)
		.def("countReal",&pyInteractionContainer::countReal,"Return number of interactions that are \"real\", i.e. they have phys and geom.")
		.def("nth",&pyInteractionContainer::pyNth,"Return n-th interaction from the container (usable for picking random interaction).")
		.def("withBody",&pyInteractionContainer::withBody,"Return list of real interactions of given body.")
		.def("withBodyAll",&pyInteractionContainer::withBodyAll,"Return list of all (real as well as non-real) interactions of given body.")
		.def("eraseNonReal",&pyInteractionContainer::eraseNonReal,"Erase all interactions that are not :yref:`real <InteractionContainer.isReal>`.")
		.def("erase",&pyInteractionContainer::erase,"Erase one interaction, given by id1, id2 (internally, ``requestErase`` is called -- the interaction might still exist as potential, if the :yref:`Collider` decides so).")
		.add_property("serializeSorted",&pyInteractionContainer::serializeSorted_get,&pyInteractionContainer::serializeSorted_set)
		.def("clear",&pyInteractionContainer::clear,"Remove all interactions, and invalidate persistent collider data (if the collider supports it).");
	python::class_<pyInteractionIterator>("InteractionIterator",python::init<pyInteractionIterator&>())
		.def("__iter__",&pyInteractionIterator::pyIter)
		.def("next",&pyInteractionIterator::pyNext);

	python::class_<pyForceContainer>("ForceContainer",python::init<pyForceContainer&>())
		.def("f",&pyForceContainer::force_get,(python::arg("id")),"Force applied on body.")
		.def("t",&pyForceContainer::torque_get,(python::arg("id")),"Torque applied on body.")
		.def("m",&pyForceContainer::torque_get,(python::arg("id")),"Deprecated alias for t (torque).")
		.def("move",&pyForceContainer::move_get,(python::arg("id")),"Displacement applied on body.")
		.def("rot",&pyForceContainer::rot_get,(python::arg("id")),"Rotation applied on body.")
		.def("addF",&pyForceContainer::force_add,(python::arg("id"),python::arg("f")),"Apply force on body (accumulates).")
		.def("addT",&pyForceContainer::torque_add,(python::arg("id"),python::arg("t")),"Apply torque on body (accumulates).")
		.def("addMove",&pyForceContainer::move_add,(python::arg("id"),python::arg("m")),"Apply displacement on body (accumulates).")
		.def("addRot",&pyForceContainer::rot_add,(python::arg("id"),python::arg("r")),"Apply rotation on body (accumulates).")
		.add_property("syncCount",&pyForceContainer::syncCount_get,&pyForceContainer::syncCount_set,"Number of synchronizations  of ForceContainer (cummulative); if significantly higher than number of steps, there might be unnecessary syncs hurting performance.")
		;

	python::class_<pyMaterialContainer>("MaterialContainer","Container for :yref:`Materials<Material>`. A material can be accessed using \n\n #. numerical index in range(0,len(cont)), like cont[2]; \n #. textual label that was given to the material, like cont['steel']. This etails traversing all materials and should not be used frequently.",python::init<pyMaterialContainer&>())
		.def("append",&pyMaterialContainer::append,"Add new shared :yref:`Material`; changes its id and return it.")
		.def("append",&pyMaterialContainer::appendList,"Append list of :yref:`Material` instances, return list of ids.")
		.def("index",&pyMaterialContainer::index,"Return id of material, given its label.")
		.def("__getitem__",&pyMaterialContainer::getitem_id)
		.def("__getitem__",&pyMaterialContainer::getitem_label)
		.def("__len__",&pyMaterialContainer::len);

	python::class_<STLImporter>("STLImporter").def("ymport",&STLImporter::import);

//////////////////////////////////////////////////////////////
///////////// proxyless wrappers 
	Serializable().pyRegisterClass(python::scope());

	python::class_<TimingDeltas, shared_ptr<TimingDeltas>, noncopyable >("TimingDeltas").add_property("data",&TimingDeltas::pyData,"Get timing data as list of tuples (label, execTime[nsec], execCount) (one tuple per checkpoint)").def("reset",&TimingDeltas::reset,"Reset timing information");

	python::scope().attr("O")=pyOmega();
}

