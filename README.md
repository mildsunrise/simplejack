# simplejack

This module enables you to register simple [JACK][] clients:

~~~ js
// Create the client
var simplejack = require("simplejack");
var client = simplejack("my-client");

// Register audio callback
client.callback(function(t) {
  return Math.sin(2*Math.PI * 440 * t);
});

// Start the client!
client.activate();
~~~

This will create a JACK client named `my-client`, with 1 audio port that
outputs a 440Hz tone. The `t` passed to the callback is the time in seconds since
the client was last activated.


## Install

First make sure you have JACK v2 development files installed. On Debian/Ubuntu and
derivatives, you can do it with:

    sudo apt-get install libjack-jackd2-dev

Then install the module as usual, e.g.

    npm install simplejack


## Options

You can pass an object to `simplejack()` in order to customize options:

~~~ js
var client = simplejack({ name: "foo", forceName: true, ports: 2 });
~~~

The available options (and their default values) are:

~~~ js
{
  // Name of the client.
  name: "simplejack",

  // If enabled and a client with the same name exists, creation
  // will fail. Otherwise a unique name will be created.
  forceName: false,

  // Explicitely connect to the server with that name.
  // Otherwise JACK will try to guess or use "default".
  server: null,

  // If the server isn't started, fail instead of starting it.
  noStartServer: null,

  // Outgoing audio ports. If this is an array of strings,
  // the strigs are the names of the created ports.
  // If an integer N is passed, then N ports will be
  // created with names "out-0", "out-1", etc.
  ports: 1,

  // Mark the ports as "terminal", meaning the audio in these
  // ports is "original" and not available on any other port.
  // Synthesizers, hardware interfaces, etc. should set this flag.
  terminal: false,

  // Mark the ports as "physical", meaning these ports correspond
  // to some kind of physical I/O connector.
  physical: false
}
~~~


## Multiple ports

If you open more than one port, the second argument passed to the callback
is the index of the port for which the sample is being produced.

So, to produce a 500Hz tone on the first port and white noise on the other, do:

~~~ js
var client = simplejack({ ports: 2 });

client.callback(function(t, port) {
  if (port === 0)
    return Math.sin(2*Math.PI * 500 * t);
  else
    return 2 * Math.random() - 1;
});
~~~


## Implementation & limitations

**Disclaimer:** yes I know Node.JS is the worst platform to do realtime sound
processing because there's a single event loop and blah, blah, blah...

A buffer is kept (always with the correct size), on which samples are precalculated
and saved. When the "process" callback gets called for the first time, zeros are
returned, and we queue UV to calculate the next samples by calling the JS callback
and save them on the buffer.

The next time the "process" callback gets called the calculated samples on the buffer
are simply copied to JACK's output buffer, and the next samples calculation is
scheduled, and so on. If for some reason the samples aren't ready when the "process"
callback is called, zeros are returned instead.

When the client is deactivated the buffer is zeroed. FIXME

One of the problems with JS is that there aren't fixed integers, it's all floating
point numbers. And I decided to pass time in seconds instead of sample number,
which makes it worse. Consequence: you will loose a bit of precision in the time,
and thus in the generated samples. I don't think it's noticeable though.



[JACK]: http://jackaudio.org "JACK Audio Connection Kit"
