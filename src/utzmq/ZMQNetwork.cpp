/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */


#include "ZMQNetwork.h"

#include <sstream>
#include <iostream>
#include <istream>


#include <utUtil/OS.h>
#include <utDataflow/ComponentFactory.h>
#include <utMeasurement/Measurement.h>

#ifdef HAVE_OPENCV
#include <utVision/Image.h>
#endif 

#include <boost/array.hpp>

#include <log4cpp/Category.hh>
#include <msgpack/v1/vrefbuffer.hpp>


namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQNetwork" ) );

// static zmq context as singleton
boost::shared_ptr<boost::asio::io_service> NetworkModule::m_ioservice;
boost::shared_ptr<boost::asio::deadline_timer> NetworkModule::m_ioserviceKeepAlive;
boost::atomic<int> NetworkModule::m_ioservice_users(0);

NetworkModule::NetworkModule( const NetworkModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory )
    : Module< NetworkModuleKey, NetworkComponentKey, NetworkModule, NetworkComponentBase >( moduleKey, pFactory )
    , m_bindTo(false)
    , m_fixTimestamp(true)
    , m_verbose(true)
    , m_serializationMethod(Serialization::PROTOCOL_MSGPACK)
    , m_has_pushsink(false)
    , m_has_pushsource(false)
    , m_has_pullsink(false)
    , m_has_pullsource(false)
    , m_receiveTimeout(500) // 1s receive timeout for now

{
    // check if correct node is available
    if( !subgraph->hasNode( "ZMQSocket" ) )
    {
        LOG4CPP_ERROR( logger, "Cannot start ZMQ module, \"ZMQSocket\"-node is missing in dfg. Please migrate your dfg using trackman or manually." );
        UBITRACK_THROW( "Cannot start ZMQ module, \"ZMQSocket\"-node is missing in dfg. Please migrate your dfg using trackman or manually." );
    }

    auto pConfig = subgraph->getNode("ZMQSocket");

    if ( pConfig->hasAttribute( "bindTo" ) )
    {
        m_bindTo = pConfig->getAttributeString( "bindTo" ) == "true";
    }
    if ( pConfig->hasAttribute( "requestTimeout" ) )
    {
        int receiveTimeout{0};
        pConfig->getAttributeData( "requestTimeout", receiveTimeout);
        m_receiveTimeout = receiveTimeout;
    }
    if ( pConfig->hasAttribute( "fixTimestamp" ) )
    {
        m_fixTimestamp = pConfig->getAttributeString( "fixTimestamp" ) == "true";
    }
    if ( pConfig->hasAttribute( "verbose" ) )
    {
        m_verbose = pConfig->getAttributeString( "verbose" ) == "true";
    }
    if ( pConfig->hasAttribute( "serialization_method" ) )
    {
        std::string sm_text = pConfig->getAttributeString( "serialization_method" );
        if (sm_text  == "boost_binary") {
            m_serializationMethod = Serialization::PROTOCOL_BOOST_BINARY;
        } else if (sm_text  == "boost_text") {
            m_serializationMethod = Serialization::PROTOCOL_BOOST_TEXT;
        } else if (sm_text  == "msgpack") {
            m_serializationMethod = Serialization::PROTOCOL_MSGPACK;
        } else {
            LOG4CPP_ERROR( logger, "Invalid Serialization Method - defaulting to Boost Binary Serialization");
        }
    }

    stopModule();
}


NetworkModule::~NetworkModule()
{
	stopModule();
}

void NetworkModule::startModule()
{
	if ( !m_running )
	{

        LOG4CPP_DEBUG( logger, "Check ZMQNetwork module configuration: " << m_moduleKey.get() );


        ComponentList allComponents( getAllComponents() );

        m_has_pushsink = false;
        m_has_pushsource = false;
        m_has_pullsink = false;
        m_has_pullsource = false;

        for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
            NetworkComponentBase::ComponentType t = (*it)->getComponentType();
            switch(t) {
            case NetworkComponentBase::PUSH_SINK:
                if (m_has_pushsource) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                if ((m_has_pullsink) || (m_has_pullsource)) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork cannot mix push and pull on one socket.");
                }

                m_has_pushsink = true;
                break;
            case NetworkComponentBase::PUSH_SOURCE:
                if (m_has_pushsink) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                if ((m_has_pullsink) || (m_has_pullsource)) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork cannot mix push and pull on one socket.");
                }

                m_has_pushsource = true;
                break;
            case NetworkComponentBase::PULL_SINK:
                if (m_has_pullsource) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                if ((m_has_pushsink) || (m_has_pushsource)) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork cannot mix push and pull on one socket.");
                }

                m_has_pullsink = true;
                break;
            case NetworkComponentBase::PULL_SOURCE:
                if (m_has_pullsink) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork has Sink and Source on one socket.");
                }
                if ((m_has_pushsink) || (m_has_pushsource)) {
                    UBITRACK_THROW("Configuration Error: ZMQNetwork cannot mix push and pull on one socket.");
                }

                m_has_pullsource = true;
                break;
            default:
                break;
            }
        }

        // select socket type
        if (m_has_pushsink) {
            m_socket_type = ZMQ_PUB;
        } else if (m_has_pushsource) {
            m_socket_type = ZMQ_SUB;
        } else if (m_has_pullsink) {
            m_socket_type = ZMQ_REP;
        } else if (m_has_pullsource) {
            m_socket_type = ZMQ_REQ;
        } else {
            UBITRACK_THROW("Configuration Error: ZMQNetwork has no Sinks or Sources.");
        }

        m_running = true;

        LOG4CPP_INFO( logger, "Starting ZMQNetwork module: " << m_moduleKey.get() );

		if (m_ioservice_users.fetch_add(1, boost::memory_order_relaxed) == 0) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			LOG4CPP_INFO( logger, "Create IOService" );
			m_ioservice.reset(new boost::asio::io_service(8));
			m_ioserviceKeepAlive.reset(new boost::asio::deadline_timer(*m_ioservice));
            watchdogTimer(boost::system::error_code());
            LOG4CPP_INFO( logger, "Start IOService Tread" );
            m_NetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &boost::asio::io_service::run, m_ioservice.get() ) ) );
		}

        initSocket();

        for ( ComponentList::iterator it = allComponents.begin(); it != allComponents.end(); it++ ) {
            (*it)->setupComponent(m_socket);
        }

        if (m_has_pushsource) {
            LOG4CPP_DEBUG( logger, "Starting listening for push messages" );
            receivePushMessage();
        } else if (m_has_pullsink) {
            LOG4CPP_DEBUG( logger, "Starting listening for pull requests" );
            handlePullRequest();
        }

        LOG4CPP_DEBUG( logger, "ZMQ Network module started" );
	}
}

void NetworkModule::stopModule()
{

	if ( m_running )
	{

		m_running = false;
        LOG4CPP_NOTICE( logger, "Stopping ZMQ Network Module" );

        ComponentList allComponents( getAllComponents() );

        for ( auto it = allComponents.begin(); it != allComponents.end(); it++ ) {
            (*it)->teardownComponent();
        }
        m_socket.reset();
		if (m_ioservice_users.fetch_sub(1, boost::memory_order_release) == 1) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			LOG4CPP_INFO( logger, "Stop IOService" );
			m_ioserviceKeepAlive->cancel();
            m_ioservice->stop();
            m_NetworkThread->join();
            m_NetworkThread.reset();
			m_ioservice.reset();
		}
	}
    LOG4CPP_DEBUG( logger, "ZMQ Network Stopped" );
}

void NetworkModule::initSocket() {
    if (m_verbose) {
        LOG4CPP_DEBUG( logger, "Resetting Socket" );
    }
    m_socket = boost::shared_ptr< azmq::socket >( new azmq::socket(*m_ioservice, m_socket_type) );

    try {
        if (m_bindTo) {
            m_socket->bind(m_moduleKey.get().c_str());
        } else {
            m_socket->connect(m_moduleKey.get().c_str());
        }
        // only for ZMQ_SUB sockets
        if (m_has_pushsource) {
            m_socket->set_option(azmq::socket::subscribe(""));
        }
        if (m_has_pullsource) {
            m_socket->set_option(m_receiveTimeout);
        }
    }
    catch (boost::system::system_error &e) {
        std::ostringstream log;
        log << "Error initializing ZMQNetwork: " << std::endl;
        log << "address: "  << m_moduleKey.get() << std::endl;
        log << e.what() << std::endl;
        LOG4CPP_ERROR( logger, log.str() );
        UBITRACK_THROW("Error Initializing ZMQNetwork");
    }
}


void NetworkModule::watchdogTimer(const boost::system::error_code& error) {
    if (error) {
        return;
    }
    if (m_verbose) {
        LOG4CPP_DEBUG( logger, "IOService Alive" );
    }
    if (m_ioservice_users > 0) {
        m_ioserviceKeepAlive->expires_from_now(boost::posix_time::seconds(1));
        m_ioserviceKeepAlive->async_wait(boost::bind(&NetworkModule::watchdogTimer, shared_from_base<NetworkModule>(), _1));
    }
}

void NetworkModule::receivePushMessage() {
    if (m_verbose) {
        LOG4CPP_DEBUG( logger, "Schedule async receive .." );
    }

    auto self(shared_from_base<NetworkModule>());
    m_socket->async_receive([this, self] (const boost::system::error_code& error, azmq::message& message, size_t bytes_transferred) {
        if (!error) {
            Measurement::Timestamp ts = Measurement::now();

            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Received " << message.size() << " bytes" );
            }
            try
            {
                LOG4CPP_TRACE(logger, "data size: " << message.size());

                if (m_serializationMethod == Serialization::PROTOCOL_BOOST_BINARY) {
                    typedef boost::iostreams::basic_array_source<char> Device;
                    boost::iostreams::stream_buffer<Device> buffer((char*)message.data(), message.size());
                    boost::archive::binary_iarchive ar_message(buffer);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::BoostArchive::deserialize(ar_message, name);
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Message for component " << name );
                    }

                    NetworkComponentKey key( name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::BoostArchive::deserialize(ar_message, measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {
                            comp->parse_boost_archive(ar_message, ts);
                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving message on ZMQPushSource - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }
                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPushSink is sending with id=\"" << name << "\", found no corresponding ZMQSource pattern with same id."  );
                    }
                } else if (m_serializationMethod == Serialization::PROTOCOL_BOOST_TEXT) {
#if __cplusplus > 201402L
                    // C__17 code allows us to not copy the data easily
                    std::istringstream buffer(std::string_view(const_cast<char*>(static_cast<const char*>(message.data())), message.size()));
#else
                    std::string message_str(static_cast<const char*>(message.data()), message.size());
                    std::istringstream buffer(message_str);
#endif
                    boost::archive::text_iarchive ar_message(buffer);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::BoostArchive::deserialize(ar_message, name);
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Message for component " << name );
                    }

                    NetworkComponentKey key( name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::BoostArchive::deserialize(ar_message, measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {
                            comp->parse_boost_archive(ar_message, ts);
                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving response on ZMQPushSource - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }
                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPushSink is sending with id=\"" << name << "\", found no corresponding ZMQSource pattern with same id."  );
                    }
                } else if (m_serializationMethod == Serialization::PROTOCOL_MSGPACK) {
                    zmq_message_unpacker pac(message);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::MsgpackArchive::deserialize(pac, name);
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Message for component " << name );
                    }

                    NetworkComponentKey key( name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::MsgpackArchive::deserialize(pac,  measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {
                            comp->parse_msgpack_archive(pac, ts);
                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving response on ZMQPushSource - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }
                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPushSink is sending with id=\"" << name << "\", found no corresponding ZMQSource pattern with same id."  );
                    }
                } else {
                    LOG4CPP_ERROR( logger, "Invalid serialization method." );
                }
            }
            catch ( const std::exception& e )
            {
                LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
            }


        } else {
            LOG4CPP_ERROR( logger, "Error receiving zmq message" << error.message());
        }

        // wait for next message
        receivePushMessage();
    });

}

void NetworkModule::handlePullRequest() {
    if (m_verbose) {
        LOG4CPP_DEBUG( logger, "Schedule async request handler" );
    }

    auto self(shared_from_base<NetworkModule>());
    m_socket->async_receive([this, self] (const boost::system::error_code& error, azmq::message& message, size_t bytes_transferred) {

        std::string suffix("\n");

        bool have_valid_request{false};
        std::string request_component_name{"undefined"};

        azmq::message snd_buf;

        if (!error) {
            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Received " << message.size() << " bytes" );
            }
            try
            {
                LOG4CPP_TRACE(logger, "data size: " << message.size());
                // handle request

                if (m_serializationMethod == Serialization::PROTOCOL_BOOST_BINARY) {
                    std::ostringstream resstream;

                    typedef boost::iostreams::basic_array_source<char> Device;
                    boost::iostreams::stream_buffer<Device> buffer((char*)message.data(), message.size());
                    boost::archive::binary_iarchive ar_message(buffer);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::BoostArchive::deserialize(ar_message, name);
                    request_component_name = name;
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Request for component " << request_component_name );
                    }

                    NetworkComponentKey key( request_component_name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::BoostArchive::deserialize(ar_message, measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {

                            Measurement::Timestamp ts(0);
                            Serialization::BoostArchive::deserialize(ar_message, ts);

                            boost::archive::binary_oarchive bpacket(resstream );
                            Serialization::BoostArchive::serialize(bpacket, request_component_name);
                            Serialization::BoostArchive::serialize(bpacket, static_cast<int>(PULL_RESPONSE_SUCCESS));
                            Serialization::BoostArchive::serialize(bpacket, static_cast<int>(comp->getMeasurementType()));
                            have_valid_request = comp->serialize_boost_archive(bpacket, ts);
                            Serialization::BoostArchive::serialize(bpacket, suffix);


                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving request on ZMQPullSink - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }

                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPullSource is requesting with id=\"" << request_component_name << "\", found no corresponding ZMQPullSink pattern with same id."  );
                    }

                    // assemble message
                    snd_buf = azmq::message(resstream.str());

                } else if (m_serializationMethod == Serialization::PROTOCOL_BOOST_TEXT) {
                    std::ostringstream resstream;

#if __cplusplus > 201402L
                    // C__17 code allows us to not copy the data easily
                    std::istringstream buffer(std::string_view(const_cast<char*>(static_cast<const char*>(message.data())), message.size()));
#else
                    std::string message_str(static_cast<const char*>(message.data()), message.size());
                    std::istringstream buffer(message_str);
#endif
                    boost::archive::text_iarchive ar_message(buffer);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::BoostArchive::deserialize(ar_message, name);
                    request_component_name = name;
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Request for component " << request_component_name );
                    }

                    NetworkComponentKey key( request_component_name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::BoostArchive::deserialize(ar_message, measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {

                            Measurement::Timestamp ts(0);
                            Serialization::BoostArchive::deserialize(ar_message, ts);

                            boost::archive::text_oarchive tpacket(resstream);
                            Serialization::BoostArchive::serialize(tpacket, request_component_name);
                            Serialization::BoostArchive::serialize(tpacket, static_cast<int>(PULL_RESPONSE_SUCCESS));
                            Serialization::BoostArchive::serialize(tpacket, static_cast<int>(comp->getMeasurementType()));
                            have_valid_request = comp->serialize_boost_archive(tpacket, ts);
                            Serialization::BoostArchive::serialize(tpacket, suffix);
                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving request on ZMQPullSink - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }
                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPullSource is requesting with id=\"" << request_component_name << "\", found no corresponding ZMQPullSink pattern with same id."  );
                    }
                    // assemble message
                    snd_buf = azmq::message(resstream.str());

                } else if (m_serializationMethod == Serialization::PROTOCOL_MSGPACK) {
                    // needed to avoid copying the message before sending...
                    auto result_buffer_ptr = new std::shared_ptr<msgpack::sbuffer>(new msgpack::sbuffer() );
                    auto result_buffer = *result_buffer_ptr;

                    zmq_message_unpacker pac(message);

                    // parse_boost_binary packet
                    std::string name;
                    Serialization::MsgpackArchive::deserialize(pac, name);
                    request_component_name = name;
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Request for component " << request_component_name );
                    }

                    NetworkComponentKey key( request_component_name );
                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< NetworkComponentBase > comp = getComponent( key );

                        int measurementType;
                        Serialization::MsgpackArchive::deserialize(pac, measurementType);
                        if (measurementType == (int)comp->getMeasurementType()) {

                            Measurement::Timestamp ts(0);
                            Serialization::MsgpackArchive::deserialize(pac, ts);

                            msgpack::packer<msgpack::sbuffer> pk(*result_buffer);
                            Serialization::MsgpackArchive::serialize(pk, request_component_name);
                            Serialization::MsgpackArchive::serialize(pk, static_cast<int>(PULL_RESPONSE_SUCCESS));
                            Serialization::MsgpackArchive::serialize(pk, static_cast<int>(comp->getMeasurementType()));
                            have_valid_request = comp->serialize_msgpack_archive(pk, ts);
                        } else {
                            LOG4CPP_ERROR( logger, "Error receiving request on ZMQPullSink - measurement types do not match" << comp->getMeasurementType() << " != " << measurementType);
                        }
                    } else if (m_verbose) {
                        LOG4CPP_WARN( logger, "ZMQPullSource is requesting with id=\"" << request_component_name << "\", found no corresponding ZMQPullSink pattern with same id."  );
                    }

                    // we're casting to a mutable buffer here in order to comply with the required azmq::message interface to provide a hint for deletion ...
                    snd_buf = azmq::message(azmq::nocopy,
                                          boost::asio::buffer(result_buffer->data(), result_buffer->size()),
                                          reinterpret_cast<void*>(result_buffer_ptr),
                                          [](void *buf, void *hint){
                                              if (hint != nullptr) {
                                                  auto b = static_cast<std::shared_ptr<msgpack::sbuffer>*>(hint);
                                                  delete b;
                                              }
                                          });

                } else {
                    LOG4CPP_ERROR( logger, "Invalid serialization method." );
                }
            }
            catch ( const std::exception& e )
            {
                LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
                if (m_verbose) {
                    LOG4CPP_TRACE(logger, printf_azmq_message_content(message));
                }
            }
        } else {
            LOG4CPP_ERROR( logger, "Error receiving zmq message" << error.message());
        }

        try {
            // Send response
            boost::system::error_code ec;
            if (have_valid_request) {

                m_socket->async_send(snd_buf, [&] (boost::system::error_code const& ec, size_t bytes_transferred) {
                    if (ec != boost::system::error_code()) {
                        LOG4CPP_ERROR( logger, "Error sending reply  on ZMQPullSink " << request_component_name << " - " << ec.message());
                    } else {
                        if (m_verbose)
                            LOG4CPP_DEBUG( logger, "Reply sent on ZMQPullSink " << request_component_name << " size: " << bytes_transferred);
                    }
                });
            } else {

                // we dont have a valid request = need to tell our client about this..
                LOG4CPP_WARN(logger, "invalid request for: " << request_component_name);

                std::ostringstream error_stream;

                if (m_serializationMethod == Serialization::PROTOCOL_BOOST_BINARY) {
                    boost::archive::binary_oarchive bpacket(error_stream );
                    Serialization::BoostArchive::serialize(bpacket, request_component_name);
                    Serialization::BoostArchive::serialize(bpacket, static_cast<int>(PULL_RESPONSE_ERROR));

                } else if (m_serializationMethod == Serialization::PROTOCOL_BOOST_TEXT) {
                    boost::archive::text_oarchive tpacket(error_stream);
                    Serialization::BoostArchive::serialize(tpacket, request_component_name);
                    Serialization::BoostArchive::serialize(tpacket, static_cast<int>(PULL_RESPONSE_ERROR));
                } else if (m_serializationMethod == Serialization::PROTOCOL_MSGPACK) {
                    msgpack::packer<std::ostringstream> pk(error_stream);
                    Serialization::MsgpackArchive::serialize(pk, request_component_name);
                    Serialization::MsgpackArchive::serialize(pk, static_cast<int>(PULL_RESPONSE_ERROR));
                } else {
                    LOG4CPP_ERROR( logger, "Invalid serialization method." );
                }

                auto snd_buf_err = boost::asio::const_buffer(error_stream.str().data(), error_stream.str().size());
                auto sz1 = m_socket->send(snd_buf_err, 0, ec);
                if (ec != boost::system::error_code()) {
                    LOG4CPP_ERROR( logger, "Error sending error message  on ZMQPullSink " << request_component_name << " - " << ec.message());
                } else {
                    if (m_verbose) {
                        LOG4CPP_DEBUG( logger, "Error message sent on ZMQPullSink " << request_component_name );
                    }
                }
            }
        }
        catch ( const std::exception& e )
        {
            LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
        }

        // wait for next message
        handlePullRequest();
    });

}


boost::shared_ptr< NetworkComponentBase > NetworkModule::createComponent( const std::string& type, const std::string& name,
    boost::shared_ptr< Graph::UTQLSubgraph > config, const NetworkModule::ComponentKey& key, NetworkModule* pModule )
{

	if ( type == "ZMQPushSourceEvent" ) // @todo should be button to be consistent or rename buttopn after all
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Button >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Distance >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourcePosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Position2D >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourcePosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Position >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourcePose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Pose >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPose >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Rotation >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Vector4D >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::Vector8D >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ButtonList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::DistanceList >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourcePositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourcePositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourcePoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::PoseList >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSourceErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSourceErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSourceCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSourceRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSourceComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

#ifdef HAVE_OPENCV
	else if (type == "ZMQPushSourceImage")
		return boost::shared_ptr< NetworkComponentBase >(new PushSourceComponent< Measurement::ImageMeasurement >(name, config, key, pModule));
#endif

    // sinks
	else if ( type == "ZMQPushSinkEvent" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Button >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Distance >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSinkPosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Position2D >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Position >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Pose >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSinkErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPose >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSinkRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Rotation >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSinkMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSinkVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Vector4D >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::Vector8D >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSinkEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ButtonList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::DistanceList >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSinkPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::PoseList >( name, config, key, pModule ) );


	else if ( type == "ZMQPushSinkErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQPushSinkErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSinkCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

	else if ( type == "ZMQPushSinkRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PushSinkComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

#ifdef HAVE_OPENCV
    else if (type == "ZMQPushSinkImage")
		return boost::shared_ptr< NetworkComponentBase >(new PushSinkComponent< Measurement::ImageMeasurement >(name, config, key, pModule));
#endif



    if ( type == "ZMQPullSourceEvent" ) // @todo should be button to be consistent or rename buttopn after all
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Button >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Distance >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourcePosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Position2D >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourcePosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Position >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourcePose" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Pose >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPose >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Rotation >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Vector4D >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::Vector8D >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ButtonList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::DistanceList >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourcePositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourcePositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::PositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourcePoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::PoseList >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSourceErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSourceErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSourceCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSourceRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSourceComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

#ifdef HAVE_OPENCV
    else if (type == "ZMQPullSourceImage")
        return boost::shared_ptr< NetworkComponentBase >(new PullSourceComponent< Measurement::ImageMeasurement >(name, config, key, pModule));
#endif

    // sinks
    else if ( type == "ZMQPullSinkEvent" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Button >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkDistance" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Distance >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSinkPosition2D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Position2D >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Position >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Pose >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSinkErrorPosition2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPosition2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkErrorPosition" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPosition >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkErrorPose" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPose >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSinkRotation" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Rotation >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSinkMatrix3x3" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkMatrix3x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkMatrix4x4" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSinkVector4D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Vector4D >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkVector8D" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::Vector8D >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSinkEventList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ButtonList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkDistanceList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::DistanceList >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSinkPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::PositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::PoseList >( name, config, key, pModule ) );


    else if ( type == "ZMQPullSinkErrorPositionList2" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPositionList2 >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkErrorPositionList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPositionList >( name, config, key, pModule ) );
    else if ( type == "ZMQPullSinkErrorPoseList" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::ErrorPoseList >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSinkCameraIntrinsics" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::CameraIntrinsics >( name, config, key, pModule ) );

    else if ( type == "ZMQPullSinkRotationVelocity" )
        return boost::shared_ptr< NetworkComponentBase >( new PullSinkComponent< Measurement::RotationVelocity >( name, config, key, pModule ) );

#ifdef HAVE_OPENCV
    else if (type == "ZMQPullSinkImage")
        return boost::shared_ptr< NetworkComponentBase >(new PullSinkComponent< Measurement::ImageMeasurement >(name, config, key, pModule));
#endif



    UBITRACK_THROW( "Class " + type + " not supported by ZMQNetwork module." );
}


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {

	// create list of supported types
    std::vector< std::string > NetworkComponents;

    NetworkComponents.push_back( "ZMQPushSourcePose" );
    NetworkComponents.push_back( "ZMQPushSourceErrorPose" );
    NetworkComponents.push_back( "ZMQPushSourceRotation" );
    NetworkComponents.push_back( "ZMQPushSourcePosition" );
    NetworkComponents.push_back( "ZMQPushSourcePosition2D" );
    NetworkComponents.push_back( "ZMQPushSourcePoseList" );
    NetworkComponents.push_back( "ZMQPushSourcePositionList" );
    NetworkComponents.push_back( "ZMQPushSourcePositionList2" );
    NetworkComponents.push_back( "ZMQPushSourceEvent" );
    NetworkComponents.push_back( "ZMQPushSourceMatrix3x3" );
    NetworkComponents.push_back( "ZMQPushSourceMatrix3x4" );
    NetworkComponents.push_back( "ZMQPushSourceMatrix4x4" );
    NetworkComponents.push_back( "ZMQPushSourceDistance" );

    NetworkComponents.push_back( "ZMQPushSourceVector4D" );
    NetworkComponents.push_back( "ZMQPushSourceVector8D" );
    NetworkComponents.push_back( "ZMQPushSourceRotationVelocity" );
    NetworkComponents.push_back( "ZMQPushSourceErrorPosition" );
    NetworkComponents.push_back( "ZMQPushSourceDistanceList" );
    NetworkComponents.push_back( "ZMQPushSourceErrorPositionList2" );
    NetworkComponents.push_back( "ZMQPushSourceErrorPositionList" );
    NetworkComponents.push_back( "ZMQPushSourceCameraIntrinsics" );
#ifdef HAVE_OPENCV
	NetworkComponents.push_back("ZMQPushSourceImage");
#endif

    NetworkComponents.push_back( "ZMQPushSinkPose" );
    NetworkComponents.push_back( "ZMQPushSinkErrorPose" );
    NetworkComponents.push_back( "ZMQPushSinkPosition" );
    NetworkComponents.push_back( "ZMQPushSinkPosition2D" );
    NetworkComponents.push_back( "ZMQPushSinkRotation" );
    NetworkComponents.push_back( "ZMQPushSinkPoseList" );
    NetworkComponents.push_back( "ZMQPushSinkPositionList" );
    NetworkComponents.push_back( "ZMQPushSinkPositionList2" );
    NetworkComponents.push_back( "ZMQPushSinkEvent" );
    NetworkComponents.push_back( "ZMQPushSinkMatrix3x3" );
    NetworkComponents.push_back( "ZMQPushSinkMatrix3x4" );
    NetworkComponents.push_back( "ZMQPushSinkMatrix4x4" );
    NetworkComponents.push_back( "ZMQPushSinkDistance" );

    NetworkComponents.push_back( "ZMQPushSinkVector4D" );
    NetworkComponents.push_back( "ZMQPushSinkVector8D" );
    NetworkComponents.push_back( "ZMQPushSinkRotationVelocity" );
    NetworkComponents.push_back( "ZMQPushSinkErrorPosition" );
    NetworkComponents.push_back( "ZMQPushSinkDistanceList" );
    NetworkComponents.push_back( "ZMQPushSinkErrorPositionList2" );
    NetworkComponents.push_back( "ZMQPushSinkErrorPositionList" );
    NetworkComponents.push_back( "ZMQPushSinkCameraIntrinsics" );
#ifdef HAVE_OPENCV    
	NetworkComponents.push_back("ZMQPushSinkImage");
#endif

    NetworkComponents.push_back( "ZMQPullSourcePose" );
    NetworkComponents.push_back( "ZMQPullSourceErrorPose" );
    NetworkComponents.push_back( "ZMQPullSourceRotation" );
    NetworkComponents.push_back( "ZMQPullSourcePosition" );
    NetworkComponents.push_back( "ZMQPullSourcePosition2D" );
    NetworkComponents.push_back( "ZMQPullSourcePoseList" );
    NetworkComponents.push_back( "ZMQPullSourcePositionList" );
    NetworkComponents.push_back( "ZMQPullSourcePositionList2" );
    NetworkComponents.push_back( "ZMQPullSourceEvent" );
    NetworkComponents.push_back( "ZMQPullSourceMatrix3x3" );
    NetworkComponents.push_back( "ZMQPullSourceMatrix3x4" );
    NetworkComponents.push_back( "ZMQPullSourceMatrix4x4" );
    NetworkComponents.push_back( "ZMQPullSourceDistance" );

    NetworkComponents.push_back( "ZMQPullSourceVector4D" );
    NetworkComponents.push_back( "ZMQPullSourceVector8D" );
    NetworkComponents.push_back( "ZMQPullSourceRotationVelocity" );
    NetworkComponents.push_back( "ZMQPullSourceErrorPosition" );
    NetworkComponents.push_back( "ZMQPullSourceDistanceList" );
    NetworkComponents.push_back( "ZMQPullSourceErrorPositionList2" );
    NetworkComponents.push_back( "ZMQPullSourceErrorPositionList" );
    NetworkComponents.push_back( "ZMQPullSourceCameraIntrinsics" );
#ifdef HAVE_OPENCV
    NetworkComponents.push_back("ZMQPullSourceImage");
#endif

    NetworkComponents.push_back( "ZMQPullSinkPose" );
    NetworkComponents.push_back( "ZMQPullSinkErrorPose" );
    NetworkComponents.push_back( "ZMQPullSinkPosition" );
    NetworkComponents.push_back( "ZMQPullSinkPosition2D" );
    NetworkComponents.push_back( "ZMQPullSinkRotation" );
    NetworkComponents.push_back( "ZMQPullSinkPoseList" );
    NetworkComponents.push_back( "ZMQPullSinkPositionList" );
    NetworkComponents.push_back( "ZMQPullSinkPositionList2" );
    NetworkComponents.push_back( "ZMQPullSinkEvent" );
    NetworkComponents.push_back( "ZMQPullSinkMatrix3x3" );
    NetworkComponents.push_back( "ZMQPullSinkMatrix3x4" );
    NetworkComponents.push_back( "ZMQPullSinkMatrix4x4" );
    NetworkComponents.push_back( "ZMQPullSinkDistance" );

    NetworkComponents.push_back( "ZMQPullSinkVector4D" );
    NetworkComponents.push_back( "ZMQPullSinkVector8D" );
    NetworkComponents.push_back( "ZMQPullSinkRotationVelocity" );
    NetworkComponents.push_back( "ZMQPullSinkErrorPosition" );
    NetworkComponents.push_back( "ZMQPullSinkDistanceList" );
    NetworkComponents.push_back( "ZMQPullSinkErrorPositionList2" );
    NetworkComponents.push_back( "ZMQPullSinkErrorPositionList" );
    NetworkComponents.push_back( "ZMQPullSinkCameraIntrinsics" );
#ifdef HAVE_OPENCV    
    NetworkComponents.push_back("ZMQPullSinkImage");
#endif

    
    cf->registerModule< NetworkModule >( NetworkComponents );
}

} } // namespace Ubitrack::Drivers
