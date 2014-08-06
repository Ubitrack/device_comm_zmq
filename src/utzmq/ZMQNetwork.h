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

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/atomic.hpp>

#include <string>
#include <cstdlib>

#include <boost/shared_ptr.hpp>
#ifdef WIN32
 #include "zmq.hpp"
#else
 #include <zmq.hpp>
#endif

#include <sstream>
#include <iostream>
#include <istream>

#include <boost/array.hpp>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>
#include <utDataflow/ComponentFactory.h>
#include <utUtil/OS.h>


#ifndef ZMQNETWORK_IOTHREADS
  #define ZMQNETWORK_IOTHREADS 2
#endif

// have a logger..
static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQNetwork" ) );


namespace Ubitrack { namespace Drivers {

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
 * owns context
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


    inline static void startReceiver(NetworkModule* pModule) {
        pModule->receiverThread();
    }
    void receiverThread();


    inline bool getFixTimestamp() {
        return m_fixTimestamp;
    }

    inline bool getVerbose() {
        return m_verbose;
    }



protected:
    static boost::shared_ptr<zmq::context_t> m_context;
	static boost::atomic<int> m_context_users;

    boost::shared_ptr<zmq::socket_t> m_socket;

    bool m_bindTo;
    bool m_fixTimestamp;
    bool m_verbose;

    boost::shared_ptr< boost::thread > m_NetworkThread;
    int m_msgwait_timeout;

    bool m_has_pushsink;
    bool m_has_pushsource;

};

/**
 * Virtual base class for all other components
 * owns the zmq socket and receives messages from it
 */
class NetworkComponentBase
    : public NetworkModule::Component
{
public:

    typedef enum { NOT_DEFINED, PUSH_SINK, PUSH_SOURCE } ComponentType;

    /** constructor */
    NetworkComponentBase( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NetworkComponentKey& componentKey, NetworkModule* pModule )
        : NetworkModule::Component( name, componentKey, pModule )
        , m_fixTimestamp(pModule->getFixTimestamp())
        , m_verbose(pModule->getVerbose())
    {}

    virtual ~NetworkComponentBase()
    {}

    virtual void setupComponent(boost::shared_ptr<zmq::socket_t> &sock)
    {
        m_socket = sock;
    }

    virtual void teardownComponent()
    {
        m_socket.reset();
    }

    virtual void parse( boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime )
    {}

    virtual NetworkComponentBase::ComponentType getComponentType() {
        // should have
        return NetworkComponentBase::NOT_DEFINED;
    }

protected:
    boost::shared_ptr<zmq::socket_t> m_socket;
    bool m_fixTimestamp;
    bool m_verbose;

};


template< class EventType >
class PushSourceComponent
    : public NetworkComponentBase
{

public:

    PushSourceComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NetworkComponentKey& key, NetworkModule* module )
        : NetworkComponentBase( name, subgraph, key, module )
        , m_port( "Output", *this )
        , m_synchronizer( 1e9 )
        , m_firstTimestamp( 0 )
    {}

    void parse( boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime )
    {
        EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
        Measurement::Timestamp sendtime;
        ar >> mm;
        ar >> sendtime;

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
            m_port.send( EventType( correctedTime, mm ) );
        } else {
            m_port.send( mm );
        }
    }

    virtual ComponentType getComponentType() {
        return NetworkComponentBase::PUSH_SOURCE;
    }

protected:
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
    PushSinkComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const NetworkComponentKey& key, NetworkModule* module )
        : NetworkComponentBase( name, subgraph, key, module )
        , m_inPort( "Input", *this, boost::bind( &PushSinkComponent::eventIn, this, _1 ) )
    {
    }

    virtual ComponentType getComponentType() {
        return NetworkComponentBase::PUSH_SINK;
    }
protected:

    // receive a new pose from the dataflow
    void eventIn( const EventType& m )
    {
        std::ostringstream stream;
        boost::archive::text_oarchive packet( stream );

        std::string suffix("\n");
        Measurement::Timestamp sendtime;

        // serialize the measurement, component name and current local time
        packet << m_name;
        packet << m;
        sendtime = Measurement::now();
        packet << sendtime;
        packet << suffix;

        zmq::message_t message(stream.str().size());
        memcpy(message.data(), stream.str().data(), stream.str().size() );

        if (m_socket) {
            bool rc = m_socket->send(message);
            LOG4CPP_DEBUG( logger, "Message sent on ZMQSink " << m_name );
            // evaluate rc
        }
    }

    // consumer port
    Dataflow::PushConsumer< EventType > m_inPort;
};





} } // namespace Ubitrack::Drivers

#endif
