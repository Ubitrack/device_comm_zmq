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

#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/binary_object.hpp>

#include "ZMQSource.h"

#include <sstream>
#include <iostream>
#include <istream>

#include <utUtil/OS.h>
#include <utDataflow/ComponentFactory.h>
#include <utMeasurement/Measurement.h>

#include <boost/array.hpp>

#include <log4cpp/Category.hh>


namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQSource" ) );

SourceModule::SourceModule( const SourceModuleKey& moduleKey, boost::shared_ptr< Graph::UTQLSubgraph >, FactoryHelper* pFactory )
	: Module< SourceModuleKey, SourceComponentKey, SourceModule, SourceComponentBase >( moduleKey, pFactory )
	, m_context(NULL)
    , m_socket(NULL)
	, m_io_threads(1)
    , m_bindTo(false)
	, m_msgwait_timeout(100) // need to find out what the resolution of the timeout is
{
	stopModule();

	Graph::UTQLSubgraph::NodePtr config;

    // read configuration like bindTo from utql config


    //	if ( subgraph->hasNode( "OptiTrack" ) )
    //	  config = subgraph->getNode( "OptiTrack" );
    //
    //	if ( !config )
    //	{
    //	  UBITRACK_THROW( "NatNetTracker Pattern has no \"OptiTrack\" node");
    //	}
    //
    //	m_clientName = config->getAttributeString( "clientName" );
    //	config->getAttributeData("bindTo", m_bindTo);

}


SourceModule::~SourceModule()
{
	stopModule();
}

void SourceModule::startModule()
{
	if ( !m_running )
	{
		m_running = true;

		LOG4CPP_DEBUG( logger, "Starting ZMQ Source module: " << m_moduleKey.get() );
//		LOG4CPP_DEBUG( logger, "Creating receiver on port " << m_moduleKey );

        m_context = new zmq::context_t(m_io_threads);
        m_socket = new zmq::socket_t(*m_context, ZMQ_SUB);

        try {
            if (m_bindTo) {
                m_socket->bind(m_moduleKey.get().c_str());
            } else {
                m_socket->connect(m_moduleKey.get().c_str());
            }
            // only for ZMQ_PUB sockets
            m_socket->setsockopt(ZMQ_SUBSCRIBE, "", 0);
        }
        catch (zmq::error_t &e) {
            std::ostringstream log;
            log << "Error initializing ZMQSource: " << std::endl;
            log << "address: "  << m_moduleKey.get() << std::endl;
            log << e.what() << std::endl;
            LOG4CPP_ERROR( logger, log.str() );
            UBITRACK_THROW("Error Initializing ZMQSource");
        }

		// network thread runs until module is stopped
		LOG4CPP_DEBUG( logger, "Starting network receiver thread" );
		m_NetworkThread = boost::shared_ptr< boost::thread >( new boost::thread( boost::bind( &SourceModule::startReceiver, this ) ) );

		LOG4CPP_DEBUG( logger, "ZMQ Source module started" );
	}
}

void SourceModule::stopModule()
{

	if ( m_running )
	{

		m_running = false;
		LOG4CPP_NOTICE( logger, "Stopping ZMQ Source Module" );

        // wait for thread
        m_NetworkThread->join();

        delete m_socket;
        m_socket = NULL;

	}
	LOG4CPP_DEBUG( logger, "ZMQ Source Stopped" );
}

void SourceModule::receiverThread() {
    int index = 0;
    ComponentList allComponents( getAllComponents() );

    zmq::pollitem_t pollitems[1];
    pollitems[0].socket = *m_socket;
    pollitems[0].events = ZMQ_POLLIN;

    // main loop
    while (m_running) {
        // wait for messages
        try {
            zmq::poll (pollitems, 1, m_msgwait_timeout);
        } catch (zmq::error_t &e) {
            std::ostringstream log;
            log << "Error polling zmq socket: " << std::endl;
            log << "address: "  << m_moduleKey.get() << std::endl;
            log << e.what() << std::endl;
            LOG4CPP_ERROR( logger, log.str() );

            Ubitrack::Util::sleep(100);

            continue;
        }
        // zmq_poll has returned
        Measurement::Timestamp ts = Measurement::now();

        zmq::message_t message;
        int flags = 0; // ZMQ_NOBLOCK
        bool rc;

        if (pollitems[0].revents & ZMQ_POLLIN) {

            if((rc = m_socket->recv(&message, flags)) == true) {
                LOG4CPP_DEBUG( logger, "Received " << message.size() << " bytes" );
                try
                {
                    std::string data( static_cast<char*>(message.data()), message.size() );
                    LOG4CPP_TRACE( logger, "data: " << data );
                    std::istringstream stream( data );
                    boost::archive::text_iarchive ar_message( stream );

                    // parse packet
                    std::string name;
                    ar_message >> name;
                    LOG4CPP_DEBUG( logger, "Message for component " << name );

                    SourceComponentKey key( name );

                    if ( hasComponent( key ) ) {
                        boost::shared_ptr< SourceComponentBase > comp = getComponent( key );
                        comp->parse( ar_message, ts );
                    }
                    else
                        LOG4CPP_WARN( logger, "ZMQSink is sending with id=\"" << name << "\", found no corresponding ZMQSource pattern with same id."  );
                }
                catch ( const std::exception& e )
                {
                    LOG4CPP_ERROR( logger, "Caught exception " << e.what() );
                }
            } else {
                LOG4CPP_ERROR( logger, "Error receiving zmq message" );
            }
            pollitems[0].revents = 0;
        }
    }
}

boost::shared_ptr< SourceComponentBase > SourceModule::createComponent( const std::string& type, const std::string& name,
	boost::shared_ptr< Graph::UTQLSubgraph > config, const SourceModule::ComponentKey& key, SourceModule* pModule )
{
	if ( type == "ZMQSourcePose" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Pose >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceErrorPose" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::ErrorPose >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceRotation" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Rotation >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePosition" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Position >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePosition2D" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Position2D >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePoseList" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PoseList >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePositionList" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PositionList >( name, config, key, pModule ) );
	else if ( type == "ZMQSourcePositionList2" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::PositionList2 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceEvent" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Button >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix3x3" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix3x3 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix3x4" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix3x4 >( name, config, key, pModule ) );
	else if ( type == "ZMQSourceMatrix4x4" )
		return boost::shared_ptr< SourceComponentBase >( new SourceComponent< Measurement::Matrix4x4 >( name, config, key, pModule ) );

	UBITRACK_THROW( "Class " + type + " not supported by network source module." );
}


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {

	// create list of supported types
	std::vector< std::string > sourceComponents;

	sourceComponents.push_back( "ZMQSourcePose" );
	sourceComponents.push_back( "ZMQSourceErrorPose" );
	sourceComponents.push_back( "ZMQSourceRotation" );
	sourceComponents.push_back( "ZMQSourcePosition" );
	sourceComponents.push_back( "ZMQSourcePosition2D" );
	sourceComponents.push_back( "ZMQSourcePoseList" );
	sourceComponents.push_back( "ZMQSourcePositionList" );
	sourceComponents.push_back( "ZMQSourcePositionList2" );
	sourceComponents.push_back( "ZMQSourceEvent" );
	sourceComponents.push_back( "ZMQSourceMatrix3x3" );
	sourceComponents.push_back( "ZMQSourceMatrix3x4" );
	sourceComponents.push_back( "ZMQSourceMatrix4x4" );

	cf->registerModule< SourceModule >( sourceComponents );
}

} } // namespace Ubitrack::Drivers
