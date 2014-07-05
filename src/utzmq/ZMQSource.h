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
        : DataflowConfigurationAttributeKey< std::string >( subgraph, "domain", "ubitrack" )
    { }
};


/**
 * Component key for zmq source.
 * Contains either the subgraph id or the value of the "senderId" dataflow attribute (if present)
 */
 class SourceComponentKey
{
public:

    SourceComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
    : m_address( "tcp://localhost:9977" )
    , m_senderId( "" )
    , m_bindTo( false )
    {
      Graph::UTQLSubgraph::EdgePtr config;

      if ( subgraph->hasEdge( "Output" ) )
          config = subgraph->getEdge( "Output" );

      if ( !config )
      {
          UBITRACK_THROW( "ZMQSource pattern has no \"Output\" edge");
      }

      config->getAttributeData( "zmqAddress", m_address );
      if ( !config->hasAttribute( "zmqSenderId" ) ) {
          m_senderId.assign(subgraph->m_ID);
      } else {
          config->getAttributeData( "zmqSenderId", m_senderId );
      }
      config->getAttributeData( "zmqBindTo", m_bindTo);

    }

    // construct from body number
    SourceComponentKey( std::string a )
        : m_address( a )
        , m_senderId( "" )
        , m_bindTo( false )
     {}

    // construct from body number and target type
    SourceComponentKey( std::string a, std::string s )
        : m_address( a )
        , m_senderId( s )
        , m_bindTo( false )
    {}

    std::string getAddress() const
    {
        return m_address;
    }

    std::string getSenderId() const
    {
        return m_senderId;
    }

    bool getBindTo() const
    {
        return m_bindTo;
    }


    // less than operator for map
    bool operator<( const SourceComponentKey& b ) const
    {
        if ( m_address.compare(b.m_address) == 0 )
            return m_senderId < b.m_senderId;
        else
            return m_address < b.m_address;
    }

protected:
    std::string m_address;
    std::string m_senderId;
    bool m_bindTo;
};

 std::ostream& operator<<( std::ostream& s, const SourceComponentKey& k );

 


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

    zmq::socket_t *createSocket(int type);

protected:
    zmq::context_t *context;
    zmq::socket_t  *signals_socket;

    int m_io_threads;
    std::string m_signals_url;

    int sendSignal(unsigned int signal, unsigned int timestamp = 0);


//    boost::shared_ptr< boost::thread > m_NetworkThread;

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

    /** component stop method */
    virtual void startComponent();

    /** component start method */
    virtual void stopComponent();


    inline const zmq::socket_t* getSocket() {
        return m_socket;
    }

protected:
    zmq::socket_t* m_socket;

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
