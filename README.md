The Bam TSV Key-Value Server
============================

Bam is an extremely minimal read-only key-value server that takes a TSV file with <key>\t<value>\n formatted lines and serves them via HTTP. It depends on the following two external libraries:

  * <a href="http://cmph.sourceforge.net/">C Minimal Perfect Hashing Library</a>
  * <a href="http://www.gnu.org/software/libmicrohttpd/">GNU libmicrohttpd</a>

The server program itself is truly minimal: it consists of under 300 lines of C code, including option parsing, help messaging, etc. To compile it just run "make". To run it just do the following:

    $ ./bam data.tsv
    Building index...
    Saving index file "test.tsv.idx"...
    Serving data @ 33.837 ms...

The service is simple: make GET requests to `/key` and you get a verbatim associated value back as the content of the HTTP response. If the key doesn't exist, you get a 404. That's it.

By default bam runs on the unprivilidged port 8080; you can use the -p option to change the port. Here's a simple usage of curl to query the value for a given key:

    $ curl http://localhost:8080/some_key
    some_key's associated value

    $ curl http://localhost:8080/invalid_key
    Resource not found


Performance
-----------

Bam is fast and simple. It mmaps the data and then builds a minimum perfect hash for the keys and uses the perfect hash to serve values directly from the mmapped data in O(1) time. Here are some performance characteristics:

  * <b>Startup:</b> it takes 30 seconds for it to start serving a 3.3G data set from a cold start on my iMac. For comparison, getting that same data set loaded on a beefy dedicated mysql machine takes about two hours of pre-processing and loading. With a pre-built index file (automatically built the first time the server is run), the same data set is served almost instantly: about 25 milliseconds.
  * <b>Latency:</b> requests are served in around 1 to 5 milliseconds per request. All bam does to serve a request is hash the key to get an index into an offset table, use the offset to get the key and value, check that the key matches, and write the value to the socket. If the working set fits in RAM, the kernel's mmap implementation takes care of everything else.
  * <b>Throughput:</b> in local load testing, bam easily manages to serve 11k requests per second, which is about what servers like memcache do. The super-low request latency helps here, but the server is also multithreaded, using one persistent kernel thread per core and a select loop within each kernel thread. There's no locking to worry about because once the data is loaded it never changes.
  * <b>Memory:</b> it's very memory-efficient — the raw TSV format is actually pretty compact; pivoting the data out into relational form usually causes about an order of magnitude inflation of the data. The only other data structures it needs are the hash data stucture, which is about 2 bits per key value, and is therefore negligible, and a table of offsets into the mmapped data, which takes 8 bytes per key-value pair.


Name
----

What does the name stand for? Nothing. I don't do acronyms. It's called that because you have data — and bam! — you're already serving it.


Sponsorship
-----------

This code is sponsored by Etsy, Inc. my employer. We're not using it as a production server, and the software is provided AS-IS with no implied or explicit warranty. Etsy is just cool enough to let me write code like this, use it for work purposes, and open source it.
