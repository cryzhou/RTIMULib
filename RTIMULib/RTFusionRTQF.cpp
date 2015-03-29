////////////////////////////////////////////////////////////////////////////
//
//  This file is part of RTIMULib
//
//  Copyright (c) 2014-2015, richards-tech
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of
//  this software and associated documentation files (the "Software"), to deal in
//  the Software without restriction, including without limitation the rights to use,
//  copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//  Software, and to permit persons to whom the Software is furnished to do so,
//  subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//  INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//  PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
//  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#include "RTFusionRTQF.h"
#include "RTIMUSettings.h"

#ifdef USE_SLERP
//  The slerp power valule controls the influence of the measured state to correct the predicted state
//  0 = measured state ignored (just gyros), 1 = measured state overrides predicted state.
//  In between 0 and 1 mixes the two conditions

#define RTQF_SLERP_POWER (RTFLOAT)0.02;

#else
//  The QVALUE affects the gyro response.

#define RTQF_QVALUE	0.001f

//  The RVALUE controls the influence of the accels and compass.
//  The bigger the value, the more sluggish the response.

#define RTQF_RVALUE	0.0005f
#endif

RTFusionRTQF::RTFusionRTQF()
{
#ifdef USE_SLERP
    m_slerpPower = RTQF_SLERP_POWER;
#else
    m_Q = RTQF_QVALUE;
    m_R = RTQF_RVALUE;
#endif
    reset();
}

RTFusionRTQF::~RTFusionRTQF()
{
}

void RTFusionRTQF::reset()
{
    m_firstTime = true;
    m_fusionPose = RTVector3();
    m_fusionQPose.fromEuler(m_fusionPose);
    m_gyro = RTVector3();
    m_accel = RTVector3();
    m_compass = RTVector3();
    m_measuredPose = RTVector3();
    m_measuredQPose.fromEuler(m_measuredPose);
    m_sampleNumber = 0;
 }

void RTFusionRTQF::predict()
{
    RTQuaternion tQuat;
    RTFLOAT x2, y2, z2;

    //  compute the state transition matrix

    x2 = m_gyro.x() / (RTFLOAT)2.0;
    y2 = m_gyro.y() / (RTFLOAT)2.0;
    z2 = m_gyro.z() / (RTFLOAT)2.0;

    m_Fk.setVal(0, 1, -x2);
    m_Fk.setVal(0, 2, -y2);
    m_Fk.setVal(0, 3, -z2);

    m_Fk.setVal(1, 0, x2);
    m_Fk.setVal(1, 2, z2);
    m_Fk.setVal(1, 3, -y2);

    m_Fk.setVal(2, 0, y2);
    m_Fk.setVal(2, 1, -z2);
    m_Fk.setVal(2, 3, x2);

    m_Fk.setVal(3, 0, z2);
    m_Fk.setVal(3, 1, y2);
    m_Fk.setVal(3, 2, -x2);

    // Predict new state

    tQuat = m_Fk * m_stateQ;
    tQuat *= m_timeDelta;
    m_stateQ += tQuat;
}


void RTFusionRTQF::update()
{
#ifdef USE_SLERP
    if (m_enableCompass || m_enableAccel) {

        // calculate rotation delta

        m_rotationDelta = m_stateQ.conjugate() * m_measuredQPose;
        m_rotationDelta.normalize();

        // take it to the power (0 to 1) to give the desired amount of correction

        RTFLOAT theta = acos(m_rotationDelta.scalar());

        RTFLOAT sinPowerTheta = sin(theta * m_slerpPower);
        RTFLOAT cosPowerTheta = cos(theta * m_slerpPower);

        m_rotationUnitVector.setX(m_rotationDelta.x());
        m_rotationUnitVector.setY(m_rotationDelta.y());
        m_rotationUnitVector.setZ(m_rotationDelta.z());
        m_rotationUnitVector.normalize();

        m_rotationPower.setScalar(cosPowerTheta);
        m_rotationPower.setX(sinPowerTheta * m_rotationUnitVector.x());
        m_rotationPower.setY(sinPowerTheta * m_rotationUnitVector.y());
        m_rotationPower.setZ(sinPowerTheta * m_rotationUnitVector.z());
        m_rotationPower.normalize();

        //  multiple this by predicted value to get result

        m_stateQ *= m_rotationPower;
    }
#else

    if (m_enableCompass || m_enableAccel) {
        m_stateQError = m_measuredQPose - m_stateQ;
    } else {
        m_stateQError = RTQuaternion();
    }

    // make new state estimate

    RTFLOAT qt = m_Q * m_timeDelta;

    m_stateQ += m_stateQError * (qt / (qt + m_R));
#endif

    m_stateQ.normalize();
}

void RTFusionRTQF::newIMUData(RTIMU_DATA& data, const RTIMUSettings *settings)
{
    if (m_debug) {
        HAL_INFO("\n------\n");
        HAL_INFO2("IMU update delta time: %f, sample %d\n", m_timeDelta, m_sampleNumber++);
    }
    m_sampleNumber++;

    if (m_enableGyro)
        m_gyro = data.gyro;
    else
        m_gyro = RTVector3();
    m_accel = data.accel;
    m_compass = data.compass;
    m_compassValid = data.compassValid;

    if (m_firstTime) {
        m_lastFusionTime = data.timestamp;
        calculatePose(m_accel, m_compass, settings->m_compassAdjDeclination);
        m_Fk.fill(0);

        //  initialize the poses

        m_stateQ.fromEuler(m_measuredPose);
        m_fusionQPose = m_stateQ;
        m_fusionPose = m_measuredPose;
        m_firstTime = false;
    } else {
        m_timeDelta = (RTFLOAT)(data.timestamp - m_lastFusionTime) / (RTFLOAT)1000000;
        m_lastFusionTime = data.timestamp;
        if (m_timeDelta <= 0)
            return;

        calculatePose(data.accel, data.compass, settings->m_compassAdjDeclination);

        predict();
        update();
        m_stateQ.toEuler(m_fusionPose);
        m_fusionQPose = m_stateQ;

        if (m_debug) {
            HAL_INFO(RTMath::displayRadians("Measured pose", m_measuredPose));
            HAL_INFO(RTMath::displayRadians("RTQF pose", m_fusionPose));
            HAL_INFO(RTMath::displayRadians("Measured quat", m_measuredPose));
            HAL_INFO(RTMath::display("RTQF quat", m_stateQ));
            HAL_INFO(RTMath::display("Error quat", m_stateQError));
         }
    }
    data.fusionPoseValid = true;
    data.fusionQPoseValid = true;
    data.fusionPose = m_fusionPose;
    data.fusionQPose = m_fusionQPose;
}
