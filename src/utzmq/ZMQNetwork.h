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


/**
 * @ingroup driver_components
 * @file
 * ZMQ Network
 * This file contains the driver component to
 * receive measurements through a zeromq network connection.
 *
 *
 * @author Ulrich Eck <ueck@net-labs.de>
 */

#ifndef _ZMQNETWORK_H_
#define _ZMQNETWORK_H_

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/atomic.hpp>


#include <string>
#include <cstdlib>

#include <boost/shared_ptr.hpp>

#include <boost/asio.hpp>
#include <azmq/socket.hpp>

#include <sstream>
#include <iostream>
#include <istream>

#include <boost/array.hpp>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/PullSupplier.h>
#include <utDataflow/PushConsumer.h>
#include <utDataflow/PullConsumer.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/MeasurementTraits.h>

#include <utMeasurement/TimestampSync.h>
#include <utDataflow/ComponentFactory.h>
#include <utUtil/OS.h>
#include <utUtil/TracingProvider.h>


#include <utSerialization/Serialization.h>
#ifdef HAVE_OPENCV
#include <utVision/ImageSerialization.h>
#endif // HAVE_OPENCV

// have a logger..
static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQNetwork" ) );


namespace Ubitrack { namespace Drivers {

enum PullResponseStatus {PULL_RESPONSE_SUCCESS, PULL_RESPONSE_ERROR};

using namespace Dataflow;

// forward declaration
class NetworkComponentBase;

class NetworkModuleKey
    : public DataflowConfigurationAttributeKey< std::string >
{
public:
    NetworkModuleKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
        : DataflowConfigurationAttributeKey< std::string >( subgraph, "socketUrl", "tcp://127.0.0.1:9977" )
    { }
};


/**
 * Component key for ZMQ Network.
 * Contains either the subgraph id or the value of the "senderId" dataflow attribute (if present)
 */
 class NetworkComponentKey
	: public std::string
{
public:
    NetworkComponentKey( const std::string& s )
		: std::string( s )
	{}

    NetworkComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
    {
		if ( !subgraph->m_DataflowAttributes.hasAttribute( "senderId" ) )
			assign( subgraph->m_ID );
		else
			assign( subgraph->m_DataflowAttributes.getAttributeString( "senderId" ) );
	}
};

/**
 * Module for ZMQ Network.
 * owns ioservice
 */
class NetworkModule
    : public Module< NetworkModuleKey, NetworkComponentKey, NetworkModule, NetworkComponentBase >
{
public:

    /** constructor */
    NetworkModule( const NetworkModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory );

    /** destruktor */
    ~NetworkModule();


    boost::shared_ptr< NetworkComponentBase > createComponent( const std::string& type, const std::string& name,
        boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const ComponentKey& key, ModuleClass* pModule );

    /** module stop method */
    virtual void startModule();

    /** module start method */
    virtual void stopModule();


    void receivePushMessage();
    void handlePullRequest();

    void watchdogTimer();

    inline bool getFixTimestamp() {
        return m_fixTimestamp;
    }

    inline bool getVerbose() {
        return m_verbose;
    }

    inline Serialization::SerializationProtocol getSerializationMethod() {
        return m_serializationMethod;
    }

protected:

    boost::shared_ptr< boost::thread > m_NetworkThread;

    // globally shared between all zmq modules
    static boost::shared_ptr<boost::asio::io_service> m_ioservice;
    static boost::shared_ptr<boost::asio::deadline_timer> m_ioserviceKeepAlive;
	static boost::atomic<int> m_ioservice_users;

    boost::shared_ptr<azmq::socket> m_socket;

    bool m_bindTo;
    bool m_fixTimestamp;
    bool m_verbose;

    Serialization::SerializationProtocol m_serializationMethod;

    bool m_has_pushsink;
    bool m_has_pushsource;
    bool m_has_pullsink;
    bool m_has_pullsource;

    azmq::socket::rcv_timeo m_receiveTimeout;

};

/**
 * Virtual base class for all other components
 * owns the zmq socket and receives messages from it
 */
class NetworkComponentBase
    : public NetworkModule::Component
{
public:

    typedef enum { NOT_DEFINED, PUSH_SINK, PUSH_SOURCE, PULL_SINK, PULL_SOURCE } ComponentType;

    /** constructor */
    NetworkComponentBase( const std::string& name, const boost::shared_ptr< Graph::UTQLSubgraph >& subgraph, const NetworkComponentKey& componentKey, NetworkModule* pModule )
        : NetworkModule::Component( name, componentKey, pModule )
        , m_fixTimestamp(pModule->getFixTimestamp())
        , m_verbose(pModule->getVerbose())
    {}

    virtual ~NetworkComponentBase() = default;

    virtual void setupComponent(boost::shared_ptr<azmq::socket> &sock)
    {
        m_socket = sock;
    }

    virtual void teardownComponent()
    {
        m_socket.reset();
    }

    virtual void parse_boost_archive(boost::archive::binary_iarchive& ar, Measurement::Timestamp recvtime)
    {}

    virtual bool serialize_boost_archive(boost::archive::binary_oarchive& ar, Measurement::Timestamp ts)
    {
        return false;
    }

	virtual void parse_boost_archive(boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime)
    {}

    virtual bool serialize_boost_archive(boost::archive::text_oarchive& ar, Measurement::Timestamp ts)
    {
        return false;
    }

#ifdef HAVE_MSGPACK
    virtual void parse_msgpack_archive(msgpack::unpacker& pac, Measurement::Timestamp recvtime)
    {}

    virtual bool serialize_msgpack_archive(msgpack::packer<std::ostringstream>& pac, Measurement::Timestamp ts)
    {
        return false;
    }
#endif

    virtual NetworkComponentBase::ComponentType getComponentType() {
        return NetworkComponentBase::NOT_DEFINED;
    }

    virtual Measurement::Traits::MeasurementType getMeasurementType() {
        return Measurement::Traits::MeasurementType::Undefined;
    }


protected:
    boost::shared_ptr<azmq::socket> m_socket;
    bool m_fixTimestamp;
    bool m_verbose;

};


template< class EventType >
class PushSourceComponent
    : public NetworkComponentBase
{

public:

    PushSourceComponent( const std::string& name, const boost::shared_ptr< Graph::UTQLSubgraph >& subgraph, const NetworkComponentKey& key, NetworkModule* module )
        : NetworkComponentBase( name, subgraph, key, module )
        , m_port( "Output", *this )
        , m_synchronizer( 1e9 )
        , m_firstTimestamp( 0 )
    {}

	void parse_boost_archive(boost::archive::binary_iarchive& ar, Measurement::Timestamp recvtime) override
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Measurement::Timestamp sendtime;
        Serialization::BoostArchive::deserialize(ar, mm);
        Serialization::BoostArchive::deserialize(ar, sendtime);

        send_message(mm, recvtime, sendtime);
    }

    void parse_boost_archive(boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime) override
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Measurement::Timestamp sendtime;
        Serialization::BoostArchive::deserialize(ar, mm);
        Serialization::BoostArchive::deserialize(ar, sendtime);

        send_message(mm, recvtime, sendtime);
    }

#ifdef HAVE_MSGPACK
    void parse_msgpack_archive(msgpack::unpacker& pac, Measurement::Timestamp recvtime) override
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Measurement::Timestamp sendtime;
        Serialization::MsgpackArchive::deserialize(pac, mm);
        Serialization::MsgpackArchive::deserialize(pac, sendtime);

        send_message(mm, recvtime, sendtime);
    }
#endif // HAVE_MSGPACK

    ComponentType getComponentType() override {
        return NetworkComponentBase::PUSH_SOURCE;
    }

    Measurement::Traits::MeasurementType getMeasurementType() override {
        return Measurement::Traits::MeasurementTypeToEnumTraits<EventType>().getMeasurementType();
    }

protected:

    void send_message(EventType& mm, Measurement::Timestamp recvtime, Measurement::Timestamp sendtime) {

        if (m_verbose) {
            LOG4CPP_DEBUG( logger, "perceived host clock offset: " << static_cast< long long >( recvtime - sendtime ) * 1e-6 << "ms" );
        }

        if (m_fixTimestamp) {
            // subtract first timestamp to avoid losing timing precision
            if ( !m_firstTimestamp )
                m_firstTimestamp = sendtime;

            // synchronize sender time with receiver time
            Measurement::Timestamp correctedTime = m_synchronizer.convertNativeToLocal( sendtime - double( m_firstTimestamp ), recvtime );

            // add offset of individual measurements
            correctedTime -= static_cast< long long >( sendtime - mm.time() );

            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Timestamps measurement: " << Measurement::timestampToShortString( mm.time() )
                        << ", sent: " << Measurement::timestampToShortString( sendtime )
                        << ", arrival: " << Measurement::timestampToShortString( recvtime )
                        << ", corrected: " << Measurement::timestampToShortString( correctedTime ) );
            }

#ifdef ENABLE_EVENT_TRACING
            TRACEPOINT_MEASUREMENT_CREATE(getEventDomain(), correctedTime, getName().c_str(), "NetworkPushSource")
#endif
            m_port.send( EventType( correctedTime, mm ) );
        } else {
#ifdef ENABLE_EVENT_TRACING
            TRACEPOINT_MEASUREMENT_CREATE(getEventDomain(), mm.time(), getName().c_str(), "NetworkPushSource")
#endif
            m_port.send( mm );
        }

    }

    PushSupplier< EventType > m_port;
    Measurement::TimestampSync m_synchronizer;
    Measurement::Timestamp m_firstTimestamp;

};


template< class EventType >
class PushSinkComponent
    : public NetworkComponentBase
{

public:

    /** constructor */
    PushSinkComponent( const std::string& name, const boost::shared_ptr< Graph::UTQLSubgraph >& subgraph, const NetworkComponentKey& key, NetworkModule* module )
        : NetworkComponentBase( name, subgraph, key, module )
        , m_inPort( "Input", *this, boost::bind( &PushSinkComponent::eventIn, this, _1 ) )
    {
    }

    ComponentType getComponentType() override {
        return NetworkComponentBase::PUSH_SINK;
    }

    Measurement::Traits::MeasurementType getMeasurementType() override {
        return Measurement::Traits::MeasurementTypeToEnumTraits<EventType>().getMeasurementType();
    }

protected:

    // receive a new pose from the dataflow
    void eventIn( const EventType& m )
    {
        // needed to avoid copying the message before sending...
        auto stream_ptr = new boost::shared_ptr<std::ostringstream>(new std::ostringstream);
        auto stream = *stream_ptr;
        std::string suffix("\n");
        Measurement::Timestamp sendtime;
        sendtime = Measurement::now();

        Serialization::SerializationProtocol sm = getModule().getSerializationMethod();
        if (sm == Serialization::SerializationProtocol::PROTOCOL_BOOST_BINARY) {
            boost::archive::binary_oarchive bpacket( *stream );

            Serialization::BoostArchive::serialize(bpacket, m_name);
            Serialization::BoostArchive::serialize(bpacket, static_cast<int>(getMeasurementType()));
            Serialization::BoostArchive::serialize(bpacket, m);
            Serialization::BoostArchive::serialize(bpacket, sendtime);
            Serialization::BoostArchive::serialize(bpacket, suffix);

        } else if (sm == Serialization::SerializationProtocol::PROTOCOL_BOOST_TEXT) {
            boost::archive::text_oarchive tpacket( *stream );

            Serialization::BoostArchive::serialize(tpacket, m_name);
            Serialization::BoostArchive::serialize(tpacket, static_cast<int>(getMeasurementType()));
            Serialization::BoostArchive::serialize(tpacket, m);
            Serialization::BoostArchive::serialize(tpacket, sendtime);
            Serialization::BoostArchive::serialize(tpacket, suffix);

#ifdef HAVE_MSGPACK
        } else if (sm == Serialization::SerializationProtocol::PROTOCOL_MSGPACK) {
            msgpack::packer<std::ostringstream> pk(stream.get());

            Serialization::MsgpackArchive::serialize(pk, m_name);
            Serialization::MsgpackArchive::serialize(pk, static_cast<int>(getMeasurementType()));
            Serialization::MsgpackArchive::serialize(pk, m);
            Serialization::MsgpackArchive::serialize(pk, sendtime);
#endif // HAVE_MSGPACK

        } else {
            LOG4CPP_ERROR( logger, "Invalid serialization protocol." );
            return;
        }

        // blackmagic .. remove const from ostringstream result without copying ..
        auto const_message_str = new std::string(stream->str().data(), stream->str().size());
        auto message_ptr = const_cast<std::string*>(const_message_str);

        azmq::message message(azmq::nocopy, boost::asio::buffer(*message_ptr), (void*)stream_ptr, [](void *buf, void *hint){
            if (hint != nullptr) {
                auto b = static_cast<std::shared_ptr<std::ostringstream>*>(hint);
                delete b;
            }
        });

        if (m_socket) {
#ifdef ENABLE_EVENT_TRACING
            TRACEPOINT_MEASUREMENT_RECEIVE(getEventDomain(), m.time(), getName().c_str(), "NetworkPushSink")
#endif
            m_socket->async_send(message, [&] (boost::system::error_code const& ec, size_t bytes_transferred) {
                if (ec != boost::system::error_code()) {
                    LOG4CPP_ERROR( logger, "Error sending message on ZMQSink " << m_name << " - " << ec.message());
                } else {
                    if (m_verbose)
                        LOG4CPP_DEBUG( logger, "Message sent on ZMQSink " << m_name );
                }
            });
            // evaluate rc
        }
    }

    // consumer port
    Dataflow::PushConsumer< EventType > m_inPort;
};



template< class EventType >
class PullSourceComponent
        : public NetworkComponentBase
{

public:

    PullSourceComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NetworkComponentKey& key, NetworkModule* module )
            : NetworkComponentBase( name, subgraph, key, module )
            , m_port( "Output", *this, boost::bind( &PullSourceComponent::request, this, _1 ) )
    {}

    EventType parse_boost_archive(boost::archive::binary_iarchive& ar)
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Serialization::BoostArchive::deserialize(ar, mm);
        return mm;
    }

    EventType parse_boost_archive(boost::archive::text_iarchive& ar)
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Serialization::BoostArchive::deserialize(ar, mm);

        return mm;
    }

#ifdef HAVE_MSGPACK
    virtual EventType parse_msgpack_archive(msgpack::unpacker& pac)
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Serialization::MsgpackArchive::deserialize(pac, mm);

        return mm;
    }
#endif // HAVE_MSGPACK

    ComponentType getComponentType() override {
        return NetworkComponentBase::PULL_SOURCE;
    }

    Measurement::Traits::MeasurementType getMeasurementType() override {
        return Measurement::Traits::MeasurementTypeToEnumTraits<EventType>().getMeasurementType();
    }

protected:

    EventType request( Measurement::Timestamp t )
    {

        boost::system::error_code ec;


        //
        // Serialize request
        //

        std::ostringstream reqstream;

        Serialization::SerializationProtocol sm = getModule().getSerializationMethod();
        if (sm == Serialization::SerializationProtocol::PROTOCOL_BOOST_BINARY) {
            boost::archive::binary_oarchive bpacket(reqstream );

            Serialization::BoostArchive::serialize(bpacket, m_name);
            Serialization::BoostArchive::serialize(bpacket, static_cast<int>(getMeasurementType()));
            Serialization::BoostArchive::serialize(bpacket, t);

        } else if (sm == Serialization::SerializationProtocol::PROTOCOL_BOOST_TEXT) {
            boost::archive::text_oarchive tpacket(reqstream );

            Serialization::BoostArchive::serialize(tpacket, m_name);
            Serialization::BoostArchive::serialize(tpacket, static_cast<int>(getMeasurementType()));
            Serialization::BoostArchive::serialize(tpacket, t);

#ifdef HAVE_MSGPACK
        } else if (sm == Serialization::SerializationProtocol::PROTOCOL_MSGPACK) {
            msgpack::packer<std::ostringstream> pk(reqstream);

            Serialization::MsgpackArchive::serialize(pk, m_name);
            Serialization::MsgpackArchive::serialize(pk, static_cast<int>(getMeasurementType()));
            Serialization::MsgpackArchive::serialize(pk, t);

#endif // HAVE_MSGPACK

        } else {
            LOG4CPP_ERROR( logger, "Invalid serialization protocol." );
            return EventType();
        }
        //
        // synchronous request for measurement
        //

        auto snd_buf = boost::asio::const_buffer(reqstream.str().data(), reqstream.str().size());
        auto sz1 = m_socket->send(snd_buf, 0, ec);
        if (ec != boost::system::error_code()) {
            LOG4CPP_ERROR( logger, "Error requesting message on ZMQSource " << m_name << " - " << ec.message());
            return EventType();
        } else {
            if (m_verbose)
                LOG4CPP_DEBUG( logger, "Message requested on ZMQSource " << m_name );
        }

#ifdef ENABLE_EVENT_TRACING
        TRACEPOINT_MEASUREMENT_CREATE(getEventDomain(), t, getName().c_str(), "NetworkPullSource")
#endif

        //
        // synchronous wait for measurement
        //
        azmq::message rcv_buf;
        auto sz2 = m_socket->receive(rcv_buf, 0, ec);
        // EAGAIN => timeout
        if (ec != boost::system::error_code()) {
            LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource " << m_name << " - " << ec.message());
            return EventType();
        } else {
            if (m_verbose)
            LOG4CPP_DEBUG( logger, "Response requested on ZMQSource " << m_name );
        }

        //
        // deserialize packet
        //

        if (sm == Serialization::PROTOCOL_BOOST_BINARY) {
            typedef boost::iostreams::basic_array_source<char> Device;
            boost::iostreams::stream_buffer<Device> buffer((char*)rcv_buf.data(), rcv_buf.size());
            boost::archive::binary_iarchive ar_message(buffer);

            // parse_boost_binary packet
            std::string name;
            Serialization::BoostArchive::deserialize(ar_message, name);
            if (name != m_name) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - names do not match" << m_name << " != " << name);
                return EventType();
            }
            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Message for component " << name );
            }

            int status;
            Serialization::BoostArchive::deserialize(ar_message, status);
            if (status != PULL_RESPONSE_SUCCESS) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - unsuccessful");
                return EventType();
            }

            int measurementType;
            Serialization::BoostArchive::deserialize(ar_message, measurementType);
            if (measurementType != (int)getMeasurementType()) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - measurement types do not match" << getMeasurementType() << " != " << measurementType);
                return EventType();
            }

            EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
            Serialization::BoostArchive::deserialize(ar_message, mm);
            // check timestamp ??
            return mm;

        } else if (sm == Serialization::PROTOCOL_BOOST_TEXT) {
            // @todo find a way to do this without copying !!!
            std::string input_data_( (char*)rcv_buf.data(), rcv_buf.size() );
            std::istringstream buffer(input_data_);
            boost::archive::text_iarchive ar_message(buffer);

            // parse_boost_text packet
            std::string name;
            Serialization::BoostArchive::deserialize(ar_message, name);
            if (name != m_name) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - names do not match" << m_name << " != " << name);
                return EventType();
            }
            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Message for component " << name );
            }

            int status;
            Serialization::BoostArchive::deserialize(ar_message, status);
            if (status != PULL_RESPONSE_SUCCESS) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - unsuccessful");
                return EventType();
            }

            int measurementType;
            Serialization::BoostArchive::deserialize(ar_message, measurementType);
            if (measurementType != (int)getMeasurementType()) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - measurement types do not match" << getMeasurementType() << " != " << measurementType);
                return EventType();
            }

            EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
            Serialization::BoostArchive::deserialize(ar_message, mm);
            // check timestamp ??
            return mm;
#ifdef HAVE_MSGPACK
        } else if (sm == Serialization::PROTOCOL_MSGPACK) {
            msgpack::unpacker pac;
            pac.reserve_buffer(rcv_buf.size());
            // @todo find a way to do this without copying !!!
            memcpy(pac.buffer(), rcv_buf.data(), rcv_buf.size() );
            pac.buffer_consumed(rcv_buf.size());


            std::string name;
            Serialization::MsgpackArchive::deserialize(pac, name);
            if (name != m_name) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - names do not match" << m_name << " != " << name);
                return EventType();
            }
            if (m_verbose) {
                LOG4CPP_DEBUG( logger, "Message for component " << name );
            }

            int status;
            Serialization::MsgpackArchive::deserialize(pac,  status);
            if (status != PULL_RESPONSE_SUCCESS) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - unsuccessful");
                return EventType();
            }

            int measurementType;
            Serialization::MsgpackArchive::deserialize(pac, measurementType);
            if (measurementType != (int)getMeasurementType()) {
                LOG4CPP_ERROR( logger, "Error receiving response on ZMQSource - measurement types do not match" << getMeasurementType() << " != " << measurementType);
                return EventType();
            }

            EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
            Serialization::MsgpackArchive::deserialize(pac, mm);
            // check timestamp ??
            return mm;
#endif // HAVE_MSGPACK
        } else {
            LOG4CPP_ERROR( logger, "Invalid serialization method." );
        }

        LOG4CPP_ERROR(logger, "ZMQPullSource - something went wrong - did not receive data");
        return EventType();
    }


    PullSupplier< EventType > m_port;

};


template< class EventType >
class PullSinkComponent
        : public NetworkComponentBase
{

public:

    /** constructor */
    PullSinkComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NetworkComponentKey& key, NetworkModule* module )
            : NetworkComponentBase( name, subgraph, key, module )
            , m_inPort( "Input", *this )
    {
    }

    ComponentType getComponentType() override {
        return NetworkComponentBase::PULL_SINK;
    }

    Measurement::Traits::MeasurementType getMeasurementType() override {
        return Measurement::Traits::MeasurementTypeToEnumTraits<EventType>().getMeasurementType();
    }


    bool serialize_boost_archive(boost::archive::binary_oarchive& ar, Measurement::Timestamp ts)
    {
        try {
            Serialization::BoostArchive::serialize(ar, m_inPort.get(ts));
            return true;
        } catch (const Util::Exception& ) {
        }
        return false;
    }

    bool serialize_boost_archive(boost::archive::text_oarchive& ar, Measurement::Timestamp ts)
    {
        try {
            Serialization::BoostArchive::serialize(ar, m_inPort.get(ts));
            return true;
        } catch (const Util::Exception& ) {
        }
        return false;
    }

#ifdef HAVE_MSGPACK
    virtual bool serialize_msgpack_archive(msgpack::packer<std::ostringstream>& pac, Measurement::Timestamp ts)
    {
        try {
            Serialization::MsgpackArchive::serialize(pac, m_inPort.get(ts));
            return true;
        } catch (const Util::Exception& ) {
        }
        return false;
    }
#endif // HAVE_MSGPACK


protected:

    // consumer port
    Dataflow::PullConsumer< EventType > m_inPort;
};





} } // namespace Ubitrack::Drivers

#endif
