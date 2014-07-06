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
 * NetworkSink component.
 *
 * @author Florian Echtler <echtler@in.tum.de>
 */

// WARNING: all boost/serialization headers should be
//          included AFTER all boost/archive headers


#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>

#include <zmq.hpp>

#include <string>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <iomanip>


#include <boost/bind.hpp>
#include <boost/thread.hpp>

#include <boost/array.hpp>
#include <boost/thread.hpp>

#include <utDataflow/PushConsumer.h>
#include <utDataflow/ComponentFactory.h>
#include <utDataflow/Component.h>
#include <utMeasurement/Measurement.h>

namespace Ubitrack { namespace Drivers {

static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.ZMQSink" ) );
	
/**
 * @ingroup dataflow_components
 * Transmits measurements over the network.
 *
 * @par Input Ports
 * PushConsumer<EventType> port "Input"
 *
 * @par Output Ports
 * None.
 *
 * @par Configuration
 * - Edge configuration:
 * @verbatim
 * <Configuration socket_url="..." destination="..."/>
 * @endverbatim
 *   - \c socket_url: the zmq socket url,default tcp://localhost:9977
 *
 * @par Instances
 * Registered for the following EventTypes and names:
 * - Ubitrack::Measurement::Position: ZMQPositionSink
 * - Ubitrack::Measurement::Rotation: ZMQRotationSink
 * - Ubitrack::Measurement::Pose : ZMQPoseSink
 */
template< class EventType >
class ZMQSinkComponent
	: public Dataflow::Component
{

public:

	/** constructor */
	ZMQSinkComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > pConfig )
		: Dataflow::Component( name )
		, m_inPort( "Input", *this, boost::bind( &ZMQSinkComponent::eventIn, this, _1 ) )
		, m_context( NULL )
		, m_socket( NULL )
		, m_socket_url( "tcp://localhost:9977" )
		, m_io_threads(1)
		, m_bindTo(true)
	{

		// check for configuration
		pConfig->m_DataflowAttributes.getAttributeData( "socketUrl", m_socket_url );
//		if ( pConfig->m_DataflowAttributes.hasAttribute( "networkDestination" ) )
//		{
//			m_Destination = pConfig->m_DataflowAttributes.getAttributeString( "networkDestination" );
//		}

        LOG4CPP_DEBUG( logger, "Initialize ZMQSink on socket: " << m_socket_url );

        m_context = new zmq::context_t(m_io_threads);
        m_socket = new zmq::socket_t(*m_context, ZMQ_PUB);

        try {
            if (m_bindTo) {
                m_socket->bind(m_socket_url.c_str());
            } else {
                m_socket->connect(m_socket_url.c_str());
            }
        }
        catch (zmq::error_t &e) {
            std::ostringstream log;
            log << "Error initializing ZMQSource: " << std::endl;
            log << "address: "  << m_socket_url << std::endl;
            log << e.what() << std::endl;
            LOG4CPP_ERROR( logger, log.str() );

            UBITRACK_THROW("Error Initializing ZMQSink");
        }

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

        bool rc = m_socket->send(message);
        LOG4CPP_DEBUG( logger, "Message sent on ZMQSink " << m_name );
        // evaluate rc
	}

	// consumer port
	Dataflow::PushConsumer< EventType > m_inPort;

	std::string m_socket_url;

    zmq::context_t *m_context;
    zmq::socket_t* m_socket;

    int m_io_threads;

    bool m_bindTo;


};


// register module at factory
UBITRACK_REGISTER_COMPONENT( Dataflow::ComponentFactory* const cf ) {
	cf->registerComponent< ZMQSinkComponent< Measurement::Pose > > ( "ZMQPoseSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::ErrorPose > > ( "ZMQErrorPoseSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Position > > ( "ZMQPositionSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Position2D > > ( "ZMQPosition2DSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Rotation > > ( "ZMQRotationSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::PoseList > > ( "ZMQPoseListSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::PositionList > > ( "ZMQPositionListSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::PositionList2 > > ( "ZMQPositionList2Sink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Button > > ( "ZMQEventSink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Matrix3x3 > > ( "ZMQMatrix3x3Sink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Matrix3x4 > > ( "ZMQMatrix3x4Sink" );
	cf->registerComponent< ZMQSinkComponent< Measurement::Matrix4x4 > > ( "ZMQMatrix4x4Sink" );
}

} } // namespace Ubitrack::Drivers

