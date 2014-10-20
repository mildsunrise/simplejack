#include <string.h>
#include <stdlib.h>

#include <sstream>
#include <exception>

#include "v8u.hpp"

#include <jack/jack.h>

class JackError : public std::exception {
public:
  jack_status_t status;
  std::string message;

  JackError(jack_status_t s, const char* when) : status(s) {
    std::stringstream msg;
    msg << "When " << when << ": JACK error code " << status;
    message = msg.str();
  }
  ~JackError() throw() {}
  const char* what() const throw() { return message.c_str(); }

  static inline void check(jack_status_t status, const char* when) throw() {
    if (status) throw JackError(status, when);
  }
  static inline void check(int status, const char* when) throw() {
    check((jack_status_t)status, when);
  }
};

class Simplejack : public node::ObjectWrap {
public:
  // User parameters
  v8::Persistent<v8::Object> callback;
  const int ports;

  // LibUV structures
  uv_async_t async_deactivate;
  uv_work_t work_req;
  uv_mutex_t period_mutex;

  // JACK structures
  jack_client_t* jack_client;
  jack_port_t** jack_ports;

  // JACK parameters
  int sampleRate;
  int bufferSize;

  // Current state
  bool holdState;
  bool periodReady;
  int ticks;
  int misses;
  jack_default_audio_sample_t** buffers;


  // [De]initialization
  // ------------------

  Simplejack(
      const char* name, bool forceName,
      std::string* portNames, int ports_, bool terminal, bool physical,
      const char *server, bool noStartServer
  ) : ports(ports_), holdState(false), ticks(0), misses(0) {
    // Setup UV structures
    uv_async_init(uv_default_loop(), &async_deactivate, &deactivate_callback);
    async_deactivate.data = this;
    work_req.data = this;
    assert(uv_mutex_init(&period_mutex) == 0);

    // Allocate arrays
    jack_ports = new jack_port_t* [ports];
    buffers = new jack_default_audio_sample_t* [ports];
    memset(buffers, 0, ports * sizeof(jack_default_audio_sample_t*));

    // Create the client
    int options = JackNullOption;
    if (forceName) options |= JackUseExactName;
    if (noStartServer) options |= JackNoStartServer;
    jack_status_t status;

    if (server) {
      options |= JackServerName;
      jack_client = jack_client_open(name, (jack_options_t)options, &status, server);
    } else {
      jack_client = jack_client_open(name, (jack_options_t)options, &status);
    }

    JackError::check(status, "opening the client");

    // Register the ports
    unsigned long port_flags = JackPortIsOutput;
    if (terminal) port_flags |= JackPortIsTerminal;
    if (physical) port_flags |= JackPortIsPhysical;

    for (int port = 0; port < ports; port++) {
      jack_ports[port] = jack_port_register(jack_client, portNames[port].c_str(), JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
      if (!jack_ports[port]) throw JackError((jack_status_t)0, "creating a port");
    }

    // Add JACK callbacks
    JackError::check(jack_set_process_callback(jack_client, process_callback, this), "setting process callback");
    JackError::check(jack_set_sample_rate_callback(jack_client, sample_rate_changed, this), "setting sample rate callback");
    JackError::check(jack_set_buffer_size_callback(jack_client, buffer_size_changed, this), "setting buffer size callback");
  }

  ~Simplejack() {
    if (!callback.IsEmpty()) callback.Dispose();
    jack_client_close(jack_client);

    for (int port = 0; port < ports; port++)
      delete[] buffers[port];
    delete[] buffers;
    delete[] jack_ports;

    uv_mutex_destroy(&period_mutex);
  }


  // JS constructor
  // --------------

  V8_CTOR() {
    // Extract the options object
    v8::Local<v8::Object> options;
    if (info[0]->IsString()) {
      options = v8u::Obj();
      options->Set(v8u::Symbol("name"), info[0]);
    } else if (info[0]->IsNumber()) {
      options = v8u::Obj();
      options->Set(v8u::Symbol("ports"), info[0]);
    } else if (info[0]->IsObject()) {
      options = v8u::Obj(info[0]);
    } else {
      options = v8u::Obj();
    }

    // Extract the parameters
    v8::Local<v8::Value> field;

    bool forceName = v8u::Bool(options->Get(v8u::Symbol("forceName")));
    bool terminal = v8u::Bool(options->Get(v8u::Symbol("terminal")));
    bool physical = v8u::Bool(options->Get(v8u::Symbol("physical")));
    bool noStartServer = v8u::Bool(options->Get(v8u::Symbol("noStartServer")));

    const char* name = "simplejack";
    field = options->Get(v8u::Symbol("name"));
    v8::String::Utf8Value nameString (field);
    if (v8u::Bool(field)) name = *nameString;

    const char *server = NULL;
    field = options->Get(v8u::Symbol("server"));
    v8::String::Utf8Value serverString (field);
    if (v8u::Bool(field)) server = *serverString;

    // Prepare port names
    std::string* portNames;
    int ports;
    field = options->Get(v8u::Symbol("ports"));
    if (!v8u::Bool(field)) field = v8u::Int(1);

    if (field->IsNumber()) {
      ports = v8u::Int(field);
      if (ports < 1 || ports > 30) V8_THROW(v8u::Err("Incorrect number of ports."));
      portNames = new std::string [ports];
      for (int port = 0; port < ports; port++) {
        std::stringstream portName;
        portName << "out-" << port;
        portNames[port] = portName.str();
      }
    } else if (field->IsArray()) {
      v8::Local<v8::Array> array = v8u::Arr(field);
      ports = array->Length();
      if (ports < 1 || ports > 30) V8_THROW(v8u::Err("Incorrect number of ports."));
      portNames = new std::string [ports];
      for (int port = 0; port < ports; port++) {
        v8::String::Utf8Value portName (array->Get(port));
        portNames[port] = std::string(*portName, portName.length());
      }
    } else {
      ports = 1;
      portNames = new std::string [ports];
      v8::String::Utf8Value portName (field);
      portNames[0] = std::string(*portName, portName.length());
    }

    V8_WRAP(new Simplejack(name, forceName, portNames, ports, terminal, physical, server, noStartServer));
  } V8_CTOR_END()


  // Getters / Setters
  // -----------------

  static V8_CB(Callback) {
    Simplejack* inst = Unwrap(info.This());

    if (!info[0]->IsObject()) V8_THROW(v8u::TypeErr("Callback must be callable"));
    v8::Handle<v8::Object> obj = v8u::Obj(info[0]);
    if (!obj->IsCallable()) V8_THROW(v8u::TypeErr("Callback must be callable"));

    uv_mutex_lock(&inst->period_mutex);
    if (!inst->callback.IsEmpty()) inst->callback.Dispose();
    inst->callback = v8::Persistent<v8::Object>::New(obj);
    inst->ticks = 0;
    uv_mutex_unlock(&inst->period_mutex);
    V8_RET(info.This());
  } V8_CB_END()

  static V8_GET(GetMisses) {
    Simplejack* inst = Unwrap(info.This());
    uv_mutex_lock(&inst->period_mutex);
    int misses = inst->misses;
    uv_mutex_unlock(&inst->period_mutex);
    V8_RET(v8u::Int(misses));
  } V8_GET_END()
  static V8_GET(GetTicks) {
    Simplejack* inst = Unwrap(info.This());
    uv_mutex_lock(&inst->period_mutex);
    int ticks = inst->ticks;
    uv_mutex_unlock(&inst->period_mutex);
    V8_RET(v8u::Int(ticks));
  } V8_GET_END()


  // Activation / deactivation
  // -------------------------

  void holdLoop() {
    if (holdState == true) return;
    holdState = true;
    uv_ref(reinterpret_cast<uv_handle_t*>(&async_deactivate));
    Ref();
  }
  void releaseLoop() {
    if (holdState == false) return;
    Unref();
    uv_unref(reinterpret_cast<uv_handle_t*>(&async_deactivate));
    holdState = false;
  }

  static void deactivate_callback(uv_async_t* handle, int status) {
    Simplejack* inst = static_cast<Simplejack*>(handle->data);
    inst->releaseLoop();
  }

  static V8_CB(Activate) {
    Simplejack* inst = Unwrap(info.This());

    inst->misses = 0;
    inst->ticks = 0;
    sample_rate_changed(jack_get_sample_rate(inst->jack_client), inst);
    buffer_size_changed(jack_get_buffer_size(inst->jack_client), inst);

    JackError::check(jack_activate(inst->jack_client), "activating the client");
    uv_queue_work(uv_default_loop(), &inst->work_req, work_empty, work_callback);

    inst->holdLoop();
    V8_RET(info.This());
  } V8_CB_END()
  static V8_CB(Deactivate) {
    Simplejack* inst = Unwrap(info.This());

    JackError::check(jack_deactivate(inst->jack_client), "deactivating the client");

    inst->releaseLoop();
    V8_RET(info.This());
  } V8_CB_END()


  // JACK main callback
  // ------------------

  static int process_callback(jack_nframes_t nframes, void *data) {
    Simplejack* inst = static_cast<Simplejack*>(data);
    uv_mutex_lock(&inst->period_mutex);

    if (inst->periodReady) {
      for (int port = 0; port < inst->ports; port++) {
        void* dest = jack_port_get_buffer(inst->jack_ports[port], nframes);
        memcpy(dest, inst->buffers[port], nframes * sizeof(jack_default_audio_sample_t));
      }
      uv_queue_work(uv_default_loop(), &inst->work_req, work_empty, work_callback);
    } else {
      inst->misses++;
    }

    inst->ticks++;
    inst->periodReady = false;

    uv_mutex_unlock(&inst->period_mutex);
    return 0;
  }


  // Other JACK callbacks
  // --------------------

  static int sample_rate_changed(jack_nframes_t nframes, void* data) {
    Simplejack* inst = static_cast<Simplejack*>(data);
    uv_mutex_lock(&inst->period_mutex);
    inst->sampleRate = nframes;
    //if (!inst->handle_.IsEmpty()) inst->handle_->Set(v8u::Symbol("sampleRate"), v8u::Int(nframes)); //FIXME
    uv_mutex_unlock(&inst->period_mutex);
    return 0;
  }

  static int buffer_size_changed(jack_nframes_t nframes, void* data) {
    Simplejack* inst = static_cast<Simplejack*>(data);
    uv_mutex_lock(&inst->period_mutex);
    inst->bufferSize = nframes;
    //if (!inst->handle_.IsEmpty()) inst->handle_->Set(v8u::Symbol("bufferSize"), v8u::Int(nframes)); //FIXME
    inst->periodReady = false;
    // Reallocate the buffers
    for (int port = 0; port < inst->ports; port++) {
      inst->buffers[port] = reinterpret_cast<jack_default_audio_sample_t*>(
        realloc(inst->buffers[port], nframes * sizeof(jack_default_audio_sample_t))
      );
    }
    uv_mutex_unlock(&inst->period_mutex);
    return 0;
  }


  // Work callback
  // -------------

  static void work_callback(uv_work_t* req, int status) {
    Simplejack* inst = static_cast<Simplejack*>(req->data);
    uv_mutex_lock(&inst->period_mutex);
    if (inst->callback.IsEmpty()) {
      uv_mutex_unlock(&inst->period_mutex);
      return;
    }

    v8::TryCatch trycatch;
    v8::Handle<v8::Value> args [3];
    v8::Handle<v8::Value> result;
    int offset = inst->bufferSize * inst->ticks;

    for (int port = 0; port < inst->ports; port++) {
      jack_default_audio_sample_t* buffer = inst->buffers[port];
      args[1] = v8u::Int(port);
      for (int i = 0; i < inst->bufferSize; i++) {
        args[0] = v8u::Num((offset + i) / (double)inst->sampleRate);
        args[2] = v8u::Int(i);
        result = inst->callback->CallAsFunction(inst->handle_, 3, args);
        if (trycatch.HasCaught()) V8_THROW(trycatch.Exception()); //FIXME: find better way, keep mutex into account
        buffer[i] = v8u::Num(result);
      }
    }

    inst->periodReady = true;
    uv_mutex_unlock(&inst->period_mutex);
  }

  // We're only interested in the "after" callback which is run in event loop
  static void work_empty(uv_work_t* req) {}


  // Type initialization
  // -------------------

  NODE_TYPE(Simplejack, "simplejack") {
    V8_DEF_CB("callback", Callback);
    V8_DEF_GET("misses", GetMisses);
    V8_DEF_GET("ticks", GetTicks);

    V8_DEF_CB("activate", Activate);
    V8_DEF_CB("deactivate", Deactivate);
    //TODO: close
  } NODE_TYPE_END()
}; V8_POST_TYPE(Simplejack);

extern "C" {
  void init(v8::Handle<v8::Object> target, v8::Handle<v8::Object> module) {
    V8_HANDLE_SCOPE(scope);

    // Initialize the main type and set it as export
    Simplejack::init(target);
    target = Simplejack::templ_->GetFunction();
    module->Set(v8u::Symbol("exports"), target);
  }
  NODE_MODULE(simplejack, init);
}
