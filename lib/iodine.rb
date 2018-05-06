require 'socket'

require 'iodine/version'
require 'iodine/iodine'

# Iodine is an HTTP / WebSocket server as well as an Evented Network Tool Library.
#
# Here is a sample Echo server using Iodine:
#
#
#       require 'iodine'
#       # define the protocol for our service
#       class EchoProtocol
#         def on_open(client)
#           # Set a connection timeout
#           client.timeout = 10
#           # Write a welcome message
#           client.write "Echo server running on Iodine #{Iodine::VERSION}.\r\n"
#         end
#         # this is called for incoming data - note data might be fragmented.
#         def on_message(client, data)
#           # write the data we received
#           client.write "echo: #{data}"
#           # close the connection when the time comes
#           client.close if data =~ /^bye[\n\r]/
#         end
#         # called if the connection is still open and the server is shutting down.
#         def on_shutdown(client)
#           # write the data we received
#           client.write "Server going away\r\n"
#         end
#       end
#       # create the service instance, the block returns a connection handler.
#       Iodine.listen(port: "3000") { EchoProtocol.new }
#       # start the service
#       Iodine.threads = 1
#       Iodine.start
#
#
#
# Methods for setting up and starting {Iodine} include {start}, {threads}, {threads=}, {workers} and {workers=}.
#
# Methods for setting startup / operational callbacks include {on_idle}, {before_fork} and {after_fork}.
#
# Methods for asynchronous execution include {run} (same as {defer}), {run_after} and {run_every}.
#
# Methods for application wide pub/sub include {subscribe}, {unsubscribe} and {publish}. Connection specific pub/sub methods are documented in the {Iodine::Connection} class).
#
# Methods for TCP/IP and Unix Sockets connections include {listen} and {connect}.
#
# Please read the {file:README.md} file for an introduction to Iodine and an overview of it's API.
#
module Iodine

    # Will monkey patch some Rack methods to increase their performance.
    def self.patch_rack
    ::Rack::Utils.class_eval do
      Iodine::Base::MonkeyPatch::RackUtils.methods(false).each do |m|
        ::Rack::Utils.define_singleton_method(m,
              Iodine::Base::MonkeyPatch::RackUtils.instance_method(m) )
        end
      end
    end

  # Will monkey patch the default JSON parser to replace the default `JSON.parse` with {Iodine::JSON.parse}.
  def self.patch_json
    ::JSON.class_eval do
        ::JSON.define_singleton_method(:parse,
              Iodine::JSON.instance_method(:parse) )
    end
  end

end


# require 'rack/handler/iodine' unless defined? ::Iodine::Rack::IODINE_RACK_LOADED
