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
 * ZMQ Source
 * This file contains the driver component to
 * receive measurements through a zeromq network connection.
 *
 *
 * @author Ulrich Eck <ueck@net-labs.de>
 */

#ifndef _ZMQSOURCE_H_
#define _ZMQSOURCE_H_

#include <string>
#include <cstdlib>

#include <zmq.hpp>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>

// have a logger..
static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQSource" ) );

namespace Ubitrack { namespace Drivers {

using namespace Dataflow;

#define ZMQSOURCE_SIGNALS_URL "inproc://zmqsourcemodule-signals"

// forward declaration
class SourceComponentBase;

/**
 * Module key for zmq source.
 * Represents the zmq context.
 */
class SourceModuleKey
    : public DataflowConfigurationAttributeKey< std::string >
{
public:
    SourceModuleKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
        : DataflowConfigurationAttributeKey< std::string >( subgraph, "address", "tcp://localhost:9977" )
    { }
};


/**
 * Component key for zmq source.
 * Contains either the subgraph id or the value of the "senderId" dataflow attribute (if present)
 */
 class SourceComponentKey
	: public std::string
{
public:
	SourceComponentKey( const std::string& s )
		: std::string( s )
	{}

	SourceComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
    {
		if ( !subgraph->m_DataflowAttributes.hasAttribute( "senderId" ) )
			assign( subgraph->m_ID );
		else
			assign( subgraph->m_DataflowAttributes.getAttributeString( "senderId" ) );
	}
};

/**
 * Module for zmq source.
 * owns context
 */
class SourceModule
    : public Module< SourceModuleKey, SourceComponentKey, SourceModule, SourceComponentBase >
{
public:

    /** constructor */
    SourceModule( const SourceModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory );

    /** destruktor */
    ~SourceModule();


    /** thread method */
//    void HandleReceive (const boost::system::error_code err, size_t length);

    boost::shared_ptr< SourceComponentBase > createComponent( const std::string& type, const std::string& name,
        boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const ComponentKey& key, ModuleClass* pModule );

    /** module stop method */
    virtual void startModule();

    /** module start method */
    virtual void stopModule();


    inline static void startReceiver(SourceModule* pModule) {
        pModule->receiverThread();
    }
    void receiverThread();

protected:

    zmq::context_t *m_context;
    zmq::socket_t* m_socket;

    int m_io_threads;

    bool m_bindTo;

    boost::shared_ptr< boost::thread > m_NetworkThread;
    int m_msgwait_timeout;


};

/**
 * Virtual base class for all other components
 * owns the zmq socket and receives messages from it
 */
class SourceComponentBase
    : public SourceModule::Component
{
public:

    /** constructor */
    SourceComponentBase( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const SourceComponentKey& componentKey, SourceModule* pModule )
        : SourceModule::Component( name, componentKey, pModule )
    {}

    virtual ~SourceComponentBase()
    {}

    virtual void parse( boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime )
    {}

};


template< class EventType >
class SourceComponent
    : public SourceComponentBase
{

public:

    SourceComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const SourceComponentKey& key, SourceModule* module )
        : SourceComponentBase( name, subgraph, key, module )
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

        LOG4CPP_DEBUG( logger, "perceived host clock offset: " << static_cast< long long >( recvtime - sendtime ) * 1e-6 << "ms" );

        // subtract first timestamp to avoid losing timing precision
        if ( !m_firstTimestamp )
            m_firstTimestamp = sendtime;

        // synchronize sender time with receiver time
        Measurement::Timestamp correctedTime = m_synchronizer.convertNativeToLocal( sendtime - double( m_firstTimestamp ), recvtime );

        // add offset of individual measurements
        correctedTime -= static_cast< long long >( sendtime - mm.time() );

        LOG4CPP_DEBUG( logger, "Timestamps measurement: " << Measurement::timestampToShortString( mm.time() )
            << ", sent: " << Measurement::timestampToShortString( sendtime )
            << ", arrival: " << Measurement::timestampToShortString( recvtime )
            << ", corrected: " << Measurement::timestampToShortString( correctedTime ) );

        m_port.send( EventType( correctedTime, mm ) );
    }

protected:
    PushSupplier< EventType > m_port;
    Measurement::TimestampSync m_synchronizer;
    Measurement::Timestamp m_firstTimestamp;
};




} } // namespace Ubitrack::Drivers

#endif
