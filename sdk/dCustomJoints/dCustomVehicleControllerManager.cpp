/* Copyright (c) <2003-2016> <Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/


#include "dCustomJointLibraryStdAfx.h"
#include "dCustomJoint.h"
#include "dCustomGear.h"
#include "dCustomHinge.h"
#include "dCustomUniversal.h"
#include "dCustomModelLoadSave.h"
#include "dCustomVehicleControllerManager.h"

//#define D_PLOT_ENGINE_CURVE


#ifdef D_PLOT_ENGINE_CURVE 
static FILE* file_xxx;
#endif

IMPLEMENT_CUSTOM_JOINT(dAxelJoint);
IMPLEMENT_CUSTOM_JOINT(dWheelJoint);
IMPLEMENT_CUSTOM_JOINT(dEngineJoint);
IMPLEMENT_CUSTOM_JOINT(dGearBoxJoint);
IMPLEMENT_CUSTOM_JOINT(dDifferentialJoint);

#define D_VEHICLE_NEUTRAL_GEAR						0
#define D_VEHICLE_REVERSE_GEAR						1
#define D_VEHICLE_FIRST_GEAR						2
#define D_VEHICLE_MAX_DRIVETRAIN_DOF				32
#define D_VEHICLE_REGULARIZER						dFloat(1.0001f)
#define D_LIMITED_SLIP_DIFFERENTIAL_LOCK_RPS		dFloat(10.0f)
#define D_VEHICLE_ENGINE_IDLE_GAS_VALVE				dFloat(0.1f)
#define D_VEHICLE_MAX_SIDESLIP_ANGLE				dFloat(35.0f * 3.1416f / 180.0f)
#define D_VEHICLE_MAX_SIDESLIP_RATE					dFloat(15.0f * 3.1416f / 180.0f)


void dEngineInfo::ConvertToMetricSystem()
{
	const dFloat horsePowerToWatts = 735.5f;
	const dFloat kmhToMetersPerSecunds = 0.278f;
	const dFloat rpmToRadiansPerSecunds = 0.105f;
	const dFloat poundFootToNewtonMeters = 1.356f;

	m_idleTorque *= poundFootToNewtonMeters;
	m_peakTorque *= poundFootToNewtonMeters;

	m_rpmAtPeakTorque *= rpmToRadiansPerSecunds;
	m_rpmAtPeakHorsePower *= rpmToRadiansPerSecunds;
	m_rpmAtRedLine *= rpmToRadiansPerSecunds;
	m_rpmAtIdleTorque *= rpmToRadiansPerSecunds;

	m_peakHorsePower *= horsePowerToWatts;
	m_vehicleTopSpeed *= kmhToMetersPerSecunds;

	m_peakPowerTorque = m_peakHorsePower / m_rpmAtPeakHorsePower;

	dAssert(m_rpmAtIdleTorque > 0.0f);
	dAssert(m_rpmAtIdleTorque < m_rpmAtPeakHorsePower);
	dAssert(m_rpmAtPeakTorque < m_rpmAtPeakHorsePower);
	dAssert(m_rpmAtPeakHorsePower < m_rpmAtRedLine);

	dAssert(m_idleTorque > 0.0f);
	dAssert(m_peakTorque > m_peakPowerTorque);
	dAssert((m_peakTorque * m_rpmAtPeakTorque) < m_peakHorsePower);
}


dEngineJoint::dEngineJoint(const dMatrix& pinAndPivotFrame, NewtonBody* const engineBody, NewtonBody* const chassisBody)
	:dCustomHinge(pinAndPivotFrame, engineBody, chassisBody)
	,m_maxRPM(50.0f)
	,m_minFriction(-10.0f)
	,m_maxFriction( 10.0f)
{
	dMatrix engineMatrix;
	dMatrix chassisMatrix;

	EnableLimits(false);
	NewtonBodyGetMatrix(engineBody, &engineMatrix[0][0]);
	NewtonBodyGetMatrix(chassisBody, &chassisMatrix[0][0]);
	m_baseOffsetMatrix = engineMatrix * chassisMatrix.Inverse();
}

void dEngineJoint::SetRedLineRPM(dFloat redLineRmp)
{
	m_maxRPM = dAbs(redLineRmp);
}

void dEngineJoint::ProjectError()
{
	dMatrix chassisMatrix;
	dVector engineOmega(0.0f);
	dVector chassisOmega(0.0f);
	dVector engineVelocity(0.0f);

	NewtonBody* const engineBody = GetBody0();
	NewtonBody* const chassisBody = GetBody1();
	dAssert (engineBody == GetEngineBody());
		
	NewtonBodyGetMatrix(chassisBody, &chassisMatrix[0][0]);
	dMatrix engineMatrix(m_baseOffsetMatrix * chassisMatrix);
	NewtonBodySetMatrixNoSleep(engineBody, &engineMatrix[0][0]);

	NewtonBodyGetPointVelocity(chassisBody, &engineMatrix.m_posit[0], &engineVelocity[0]);
	NewtonBodySetVelocityNoSleep(engineBody, &engineVelocity[0]);

	NewtonBodyGetOmega(engineBody, &engineOmega[0]);
	NewtonBodyGetOmega(chassisBody, &chassisOmega[0]);

	chassisMatrix = GetMatrix1() * chassisMatrix;
	dVector projectOmega(chassisMatrix.m_front.Scale(engineOmega.DotProduct3(chassisMatrix.m_front)) +
							chassisMatrix.m_up.Scale(chassisOmega.DotProduct3(chassisMatrix.m_up)) +
							chassisMatrix.m_right.Scale(chassisOmega.DotProduct3(chassisMatrix.m_right)));
	NewtonBodySetOmegaNoSleep(engineBody, &projectOmega[0]);
}

void dEngineJoint::SubmitConstraintsFreeDof(dFloat timestep, const dMatrix& matrix0, const dMatrix& matrix1)
{
	dFloat alpha = m_jointOmega / timestep;
	NewtonUserJointAddAngularRow(m_joint, 0, &matrix1.m_front[0]);
	NewtonUserJointSetRowAcceleration(m_joint, -alpha);
	if (m_jointOmega <= 0.0f) {
		NewtonUserJointSetRowMinimumFriction(m_joint, 0.0f);
	} else if (m_jointOmega >= m_maxRPM) {
		dFloat redLineAlpha = (m_maxRPM - m_jointOmega) / timestep;
		NewtonUserJointSetRowAcceleration(m_joint, redLineAlpha);
		NewtonUserJointSetRowMaximumFriction(m_joint, 0.0f);
	} else {
		NewtonUserJointSetRowMinimumFriction(m_joint, m_minFriction);
		NewtonUserJointSetRowMaximumFriction(m_joint, m_maxFriction);
	}
	NewtonUserJointSetRowStiffness(m_joint, 1.0f);
}

void dEngineJoint::Load(dCustomJointSaveLoad* const fileLoader)
{
	const char* token = fileLoader->NextToken();
	dAssert(!strcmp(token, "maxRPM:"));
	m_maxRPM = fileLoader->LoadFloat();

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "baseOffsetPosit:"));
	dVector posit0(fileLoader->LoadVector());
	posit0.m_w = 1.0f;

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "baseOffsetRotation:"));
	dVector euler0(fileLoader->LoadVector());
	euler0 = euler0.Scale(3.14159213f / 180.0f);
	m_baseOffsetMatrix = dMatrix(euler0.m_x, euler0.m_y, euler0.m_z, posit0);
}

void dEngineJoint::Save(dCustomJointSaveLoad* const fileSaver) const
{
	dVector euler0;
	dVector euler1;
	dCustomHinge::Save (fileSaver);

	m_baseOffsetMatrix.GetEulerAngles(euler0, euler1);
	euler0 = euler0.Scale (180.0f / 3.141592f);
	fileSaver->SaveFloat("\tmaxRPM", m_maxRPM);
	fileSaver->SaveVector("\tbaseOffsetPosit", m_baseOffsetMatrix.m_posit);
	fileSaver->SaveVector("\tbaseOffsetRotation", euler0);
}


dDifferentialJoint::dDifferentialJoint(const dMatrix& pinAndPivotFrame, NewtonBody* const differentialBody, NewtonBody* const chassisBody)
	:dCustomUniversal(pinAndPivotFrame, differentialBody, chassisBody)
//		,m_slipDifferentialSpeed(0.0f)
//		,m_slipDifferentialOn(true)
	,m_turnSpeed(0.0f)
	,m_isTractionDifferential(false)
{
	dMatrix engineMatrix;
	dMatrix chassisMatrix;

	EnableLimit_0(false);
	EnableLimit_1(false);
	NewtonBodyGetMatrix(differentialBody, &engineMatrix[0][0]);
	NewtonBodyGetMatrix(chassisBody, &chassisMatrix[0][0]);
	m_baseOffsetMatrix = engineMatrix * chassisMatrix.Inverse();
}

void dDifferentialJoint::ProjectError()
{
	dMatrix chassisMatrix;
	dVector chassisOmega(0.0f);
	dVector differentialOmega(0.0f);
	dVector differentialVelocity(0.0f);

	NewtonBody* const chassisBody = GetBody1();
	NewtonBody* const differentialBody = GetBody0();

	NewtonBodyGetMatrix(chassisBody, &chassisMatrix[0][0]);
	dMatrix differentialMatrix(m_baseOffsetMatrix * chassisMatrix);
	NewtonBodySetMatrixNoSleep(differentialBody, &differentialMatrix[0][0]);

	NewtonBodyGetPointVelocity(chassisBody, &differentialMatrix.m_posit[0], &differentialVelocity[0]);
	NewtonBodySetVelocityNoSleep(differentialBody, &differentialVelocity[0]);

	NewtonBodyGetOmega(chassisBody, &chassisOmega[0]);
	NewtonBodyGetOmega(differentialBody, &differentialOmega[0]);

	chassisMatrix = GetMatrix1() * chassisMatrix;
	dVector projectOmega(chassisMatrix.m_front.Scale(differentialOmega.DotProduct3(chassisMatrix.m_front)) +
							chassisMatrix.m_up.Scale(differentialOmega.DotProduct3(chassisMatrix.m_up)) +
							chassisMatrix.m_right.Scale(chassisOmega.DotProduct3(chassisMatrix.m_right)));
	NewtonBodySetOmegaNoSleep(differentialBody, &differentialOmega[0]);
}

void dDifferentialJoint::SubmitConstraints(dFloat timestep, int threadIndex)
{
	dMatrix chassisMatrix;
	dMatrix differentialMatrix;
	dVector chassisOmega(0.0f);
	dVector differentialOmega(0.0f);

	dCustomUniversal::SubmitConstraints(timestep, threadIndex);

	// y axis controls the slip differential feature.
	NewtonBody* const chassisBody = GetBody1();
	NewtonBody* const diffentialBody = GetBody0();

	NewtonBodyGetOmega(diffentialBody, &differentialOmega[0]);
	NewtonBodyGetOmega(chassisBody, &chassisOmega[0]);

	// calculate the position of the pivot point and the Jacobian direction vectors, in global space. 
	CalculateGlobalMatrix(differentialMatrix, chassisMatrix);
	dVector relOmega(differentialOmega - chassisOmega);

	// apply differential
	dFloat differentailOmega = differentialMatrix.m_front.DotProduct3(relOmega);

	if (m_isTractionDifferential) {
		dFloat wAlpha = (m_turnSpeed - differentailOmega) / timestep;
		NewtonUserJointAddAngularRow(m_joint, 0.0f, &differentialMatrix.m_front[0]);
		NewtonUserJointSetRowAcceleration(m_joint, wAlpha);
	} else {
		if (differentailOmega > D_LIMITED_SLIP_DIFFERENTIAL_LOCK_RPS) {
			dFloat wAlpha = (D_LIMITED_SLIP_DIFFERENTIAL_LOCK_RPS - differentailOmega) / timestep;
			NewtonUserJointAddAngularRow(m_joint, 0.0f, &differentialMatrix.m_front[0]);
			NewtonUserJointSetRowAcceleration(m_joint, wAlpha);
			NewtonUserJointSetRowMaximumFriction(m_joint, 0.0f);
		} else if (differentailOmega < -D_LIMITED_SLIP_DIFFERENTIAL_LOCK_RPS) {
			dFloat wAlpha = (-D_LIMITED_SLIP_DIFFERENTIAL_LOCK_RPS - differentailOmega) / timestep;
			NewtonUserJointAddAngularRow(m_joint, 0.0f, &differentialMatrix.m_front[0]);
			NewtonUserJointSetRowAcceleration(m_joint, wAlpha);
			NewtonUserJointSetRowMinimumFriction(m_joint, 0.0f);
		}
	}
}

void dDifferentialJoint::Load(dCustomJointSaveLoad* const fileLoader)
{
	const char* token = fileLoader->NextToken();
	dAssert(!strcmp(token, "baseOffset_position:"));
	dVector posit0(fileLoader->LoadVector());
	posit0.m_w = 1.0f;

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "baseOffset_rotation:"));
	dVector euler0(fileLoader->LoadVector());
	euler0 = euler0.Scale(3.14159213f / 180.0f);

	m_baseOffsetMatrix = dMatrix (euler0.m_x, euler0.m_y, euler0.m_z, posit0);

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "turnSpeed:"));
	m_turnSpeed = fileLoader->LoadFloat();

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "isTractionDifferential:"));
	m_isTractionDifferential = fileLoader->LoadInt() ? true : false;
}

void dDifferentialJoint::Save(dCustomJointSaveLoad* const fileSaver) const
{
	// nothing really to save;
	dCustomUniversal::Save(fileSaver);

	dVector euler0;
	dVector euler1;
		
	m_baseOffsetMatrix.GetEulerAngles(euler0, euler1);
	euler0 = euler0.Scale(180.0f / 3.141592f);

	fileSaver->SaveVector("\tbaseOffset_position", m_baseOffsetMatrix.m_posit);
	fileSaver->SaveVector("\tbaseOffset_rotation", euler0);

	fileSaver->SaveFloat ("\tturnSpeed", m_turnSpeed);
	fileSaver->SaveInt ("\tisTractionDifferential", int (m_isTractionDifferential));
}

dWheelJoint::dWheelJoint(const dMatrix& pinAndPivotFrame, NewtonBody* const tireBody, NewtonBody* const chassisBody, dCustomVehicleController* const controller, const dTireInfo& tireInfo)
	:dCustomJoint(6, tireBody, chassisBody)
	,m_lateralDir(0.0f)
	,m_longitudinalDir(0.0f)
	,m_tireLoad(0.0f)
	,m_radio(tireInfo.m_radio)
	,m_steerRate(0.5f * 3.1416f)
	,m_steerAngle0(0.0f)
	,m_steerAngle1(0.0f)
	,m_brakeTorque(0.0f)
	,m_dampingRatio(tireInfo.m_dampingRatio)
	,m_springStrength(tireInfo.m_springStrength)
	,m_suspensionLength(tireInfo.m_suspensionLength)
	,m_lateralSlip(0.0f)
	,m_aligningTorque(0.0f)
	,m_longitudinalSlip(0.0f)
	,m_aligningMomentTrail(tireInfo.m_aligningMomentTrail)
	,m_lateralStiffness(tireInfo.m_lateralStiffness)
	,m_longitudialStiffness(tireInfo.m_longitudialStiffness)
	,m_maxSteeringAngle(tireInfo.m_maxSteeringAngle)
	,m_controller(controller)
	,m_suspentionType(m_offroad)
	,m_hasFender(tireInfo.m_hasFender)
	,m_collidingCount(0)
{
	CalculateLocalMatrix(pinAndPivotFrame, m_localMatrix0, m_localMatrix1);
}


dFloat dWheelJoint::CalculateTireParametricPosition(const dMatrix& tireMatrix, const dMatrix& chassisMatrix) const
{
	const dVector& chassisP0 = chassisMatrix.m_posit;
	dVector chassisP1(chassisMatrix.m_posit + chassisMatrix.m_up.Scale(m_suspensionLength));
	dVector p1p0(chassisP1 - chassisP0);
	dVector q1p0(tireMatrix.m_posit - chassisP0);
	dFloat num = q1p0.DotProduct3(p1p0);
	dFloat den = p1p0.DotProduct3(p1p0);
	return num / den;
}

void dWheelJoint::ProjectError()
{
	dMatrix tireMatrix;
	dMatrix chassisMatrix;
	dVector tireVeloc(0.0f);
	dVector tireOmega(0.0f);
	dVector chassisVeloc(0.0f);
	dVector chassisOmega(0.0f);

	NewtonBody* const tire = m_body0;
	NewtonBody* const chassis = m_body1;
	dAssert(m_body0 == GetTireBody());

	CalculateGlobalMatrix(tireMatrix, chassisMatrix);
	chassisMatrix = dYawMatrix(m_steerAngle0) * chassisMatrix;

	tireMatrix.m_front = chassisMatrix.m_front;
	tireMatrix.m_right = tireMatrix.m_front.CrossProduct(tireMatrix.m_up);
	tireMatrix.m_right = tireMatrix.m_right.Scale(1.0f / dSqrt(tireMatrix.m_right.DotProduct3(tireMatrix.m_right)));
	tireMatrix.m_up = tireMatrix.m_right.CrossProduct(tireMatrix.m_front);

	dVector projectPosition(chassisMatrix.m_up.Scale(tireMatrix.m_posit.DotProduct3(chassisMatrix.m_up)) +
							chassisMatrix.m_front.Scale(chassisMatrix.m_posit.DotProduct3(chassisMatrix.m_front)) +
							chassisMatrix.m_right.Scale(chassisMatrix.m_posit.DotProduct3(chassisMatrix.m_right)));
	tireMatrix.m_posit = projectPosition;

	tireMatrix = GetMatrix0().Inverse() * tireMatrix;
	NewtonBodySetMatrixNoSleep(tire, &tireMatrix[0][0]);

	NewtonBodyGetVelocity(tire, &tireVeloc[0]);
	NewtonBodyGetPointVelocity(chassis, &tireMatrix.m_posit[0], &chassisVeloc[0]);
	dVector projectVelocity(chassisMatrix.m_up.Scale(tireVeloc.DotProduct3(chassisMatrix.m_up)) +
							chassisMatrix.m_front.Scale(chassisVeloc.DotProduct3(chassisMatrix.m_front)) +
							chassisMatrix.m_right.Scale(chassisVeloc.DotProduct3(chassisMatrix.m_right)));
	NewtonBodySetVelocityNoSleep(tire, &projectVelocity[0]);

	NewtonBodyGetOmega(tire, &tireOmega[0]);
	NewtonBodyGetOmega(chassis, &chassisOmega[0]);
	dVector projectOmega(chassisMatrix.m_front.Scale(tireOmega.DotProduct3(chassisMatrix.m_front)) +
							chassisMatrix.m_up.Scale(chassisOmega.DotProduct3(chassisMatrix.m_up)) +
							chassisMatrix.m_right.Scale(chassisOmega.DotProduct3(chassisMatrix.m_right)));
	NewtonBodySetOmegaNoSleep(tire, &projectOmega[0]);
}

void dWheelJoint::SubmitConstraints(dFloat timestep, int threadIndex)
{
	dMatrix tireMatrix;
	dMatrix chassisMatrix;

	NewtonBody* const tire = m_body0;
	NewtonBody* const chassis = m_body1;
	//dAssert(!m_tire || m_body0 == m_tire->GetBody());
	//dAssert(!m_tire || m_body1 == m_tire->GetParent()->GetBody());

	// calculate the position of the pivot point and the Jacobian direction vectors, in global space. 
	CalculateGlobalMatrix(tireMatrix, chassisMatrix);
	chassisMatrix = dYawMatrix(m_steerAngle0) * chassisMatrix;

	m_lateralDir = chassisMatrix.m_front;
	m_longitudinalDir = chassisMatrix.m_right;

	NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &m_lateralDir[0]);
	NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));

	NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &m_longitudinalDir[0]);
	NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));

	dFloat angle = -CalculateAngle(tireMatrix.m_front, chassisMatrix.m_front, chassisMatrix.m_right);
	NewtonUserJointAddAngularRow(m_joint, -angle, &chassisMatrix.m_right[0]);
	NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));

	angle = -CalculateAngle(tireMatrix.m_front, chassisMatrix.m_front, chassisMatrix.m_up);
	NewtonUserJointAddAngularRow(m_joint, -angle, &chassisMatrix.m_up[0]);
	NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));

	dFloat param = CalculateTireParametricPosition(tireMatrix, chassisMatrix);
	if (param >= 1.0f) {
		dVector posit(chassisMatrix.m_posit + chassisMatrix.m_up.Scale (m_suspensionLength));
		NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &posit[0], &chassisMatrix.m_up[0]);
		NewtonUserJointSetRowMaximumFriction(m_joint, 0.0f);
	} else if (param <= 0.0f) {
		NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &chassisMatrix.m_up[0]);
		NewtonUserJointSetRowMinimumFriction(m_joint, 0.0f);
	} else if (m_suspensionLength == m_roller) {
		dAssert(0);
		NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &chassisMatrix.m_up[0]);
	}

	if (m_brakeTorque > 1.0e-3f) {
		dVector tireOmega(0.0f);
		dVector chassisOmega(0.0f);
		NewtonBodyGetOmega(tire, &tireOmega[0]);
		NewtonBodyGetOmega(chassis, &chassisOmega[0]);
		dVector relOmega(tireOmega - chassisOmega);

		dFloat speed = relOmega.DotProduct3(m_lateralDir);
		NewtonUserJointAddAngularRow(m_joint, 0.0f, &m_lateralDir[0]);
		NewtonUserJointSetRowAcceleration(m_joint, -speed / timestep);
		NewtonUserJointSetRowMinimumFriction(m_joint, -m_brakeTorque);
		NewtonUserJointSetRowMaximumFriction(m_joint, m_brakeTorque);
	}

	m_brakeTorque = 0.0f;
}


void dWheelJoint::Load(dCustomJointSaveLoad* const fileLoader)
{
dAssert (0);
/*
	m_tire = NULL;
	m_lateralDir = dVector (0.0f);
	m_longitudinalDir = dVector (0.0f);
	m_tireLoad = 0.0f;
	m_steerRate = 0.0f;
	m_steerAngle0 = 0.0f;
	m_steerAngle1 = 0.0f;
	m_brakeTorque = 0.0f;

	const char* token = fileLoader->NextToken();
	dAssert(!strcmp(token, "suspensionLength:"));
	m_suspensionLength = fileLoader->LoadFloat();

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "suspentionType:"));
	m_suspentionType = dBodyPartTire::dInfo::SuspensionType(fileLoader->LoadInt());
*/
}

void dWheelJoint::Save(dCustomJointSaveLoad* const fileSaver) const
{
dAssert (0);
//		dCustomJoint::Save(fileSaver);
//		fileSaver->SaveFloat("\tsuspensionLength", m_suspensionLength);
//		fileSaver->SaveInt("suspentionType", m_suspentionType);
}

void dWheelJoint::Debug(dDebugDisplay* const debugDisplay) const
{
	dCustomJoint::Debug(debugDisplay);

	dMatrix matrix0;
	dMatrix matrix1;
	CalculateGlobalMatrix(matrix0, matrix1);
	debugDisplay->DrawFrame(matrix0);
	debugDisplay->DrawFrame(matrix1);
}

dGearBoxJoint::dGearBoxJoint(const dVector& childPin, NewtonBody* const differential, NewtonBody* const engine, dFloat maxFrictionToque)
	:dCustomGear(1, differential, engine)
	,m_param (1.0f)
	,m_cluthFrictionTorque (maxFrictionToque)
{
	dMatrix pinAndPivotFrame(dGrammSchmidt(childPin));
	CalculateLocalMatrix(pinAndPivotFrame, m_localMatrix0, m_localMatrix1);
}


void dGearBoxJoint::SubmitConstraints(dFloat timestep, int threadIndex)
{
//xxxxx
m_param = 0.0f;
	if (m_param > 0.1f) {
		dCustomGear::SubmitConstraints(timestep, threadIndex);
		if (m_param < 0.9f) {
			NewtonUserJointSetRowMinimumFriction(m_joint, -m_param * m_cluthFrictionTorque);
			NewtonUserJointSetRowMaximumFriction(m_joint,  m_param * m_cluthFrictionTorque);
		}
	}
}

void dGearBoxJoint::Load(dCustomJointSaveLoad* const fileLoader)
{
	m_param = 0.0f;
#ifdef _DEBUG
	const char* token = fileLoader->NextToken();
	dAssert(!strcmp(token, "clutchFrictionTorque:"));
#else
	fileLoader->NextToken();
#endif

	m_cluthFrictionTorque = fileLoader->LoadFloat();
}

void dGearBoxJoint::Save(dCustomJointSaveLoad* const fileSaver) const
{
	dCustomGear::Save(fileSaver);
	fileSaver->SaveFloat("\tclutchFrictionTorque", m_cluthFrictionTorque);
}

dAxelJoint::dAxelJoint(const dVector& childPin, const dVector& parentPin, const dVector& referencePin, NewtonBody* const child, NewtonBody* const parent, NewtonBody* const parentReference)
	:dCustomGear(1.0f, childPin, parentPin, child, parent)
	,m_parentReference(parentReference)
{
	dMatrix dommyMatrix;
	// calculate the local matrix for body body0
 	dMatrix pinAndPivot0(dGrammSchmidt(childPin));

	CalculateLocalMatrix(pinAndPivot0, m_localMatrix0, dommyMatrix);
	m_localMatrix0.m_posit = dVector(0.0f, 0.0f, 0.0f, 1.0f);

	// calculate the local matrix for body body1  
	dMatrix pinAndPivot1(dGrammSchmidt(parentPin));
	CalculateLocalMatrix(pinAndPivot1, dommyMatrix, m_localMatrix1);
	m_localMatrix1.m_posit = dVector(0.0f, 0.0f, 0.0f, 1.0f);

	dMatrix referenceMatrix;
	NewtonBodyGetMatrix(m_parentReference, &referenceMatrix[0][0]);
	m_pintOnReference = referenceMatrix.UnrotateVector(referencePin);
}


void dAxelJoint::SubmitConstraints(dFloat timestep, int threadIndex)
{
	dMatrix matrix0;
	dMatrix matrix1;
	dMatrix referenceMatrix;
	dVector omega0(0.0f);
	dVector omega1(0.0f);
	dFloat jacobian0[6];
	dFloat jacobian1[6];

	dAssert (m_parentReference);
	// calculate the position of the pivot point and the Jacobian direction vectors, in global space. 
	CalculateGlobalMatrix(matrix0, matrix1);
	NewtonBodyGetMatrix(m_parentReference, &referenceMatrix[0][0]);

	// calculate the angular velocity for both bodies
	dVector dir0(matrix0.m_front);
	dVector dir2(matrix1.m_front);
	dVector dir3(referenceMatrix.RotateVector(m_pintOnReference));
	dVector dir1(dir2 + dir3);

	jacobian0[0] = 0.0f;
	jacobian0[1] = 0.0f;
	jacobian0[2] = 0.0f;
	jacobian0[3] = dir0.m_x;
	jacobian0[4] = dir0.m_y;
	jacobian0[5] = dir0.m_z;

	jacobian1[0] = 0.0f;
	jacobian1[1] = 0.0f;
	jacobian1[2] = 0.0f;
	jacobian1[3] = dir1.m_x;
	jacobian1[4] = dir1.m_y;
	jacobian1[5] = dir1.m_z;

	NewtonBodyGetOmega(m_body0, &omega0[0]);
	NewtonBodyGetOmega(m_body1, &omega1[0]);

	dFloat w0 = omega0.DotProduct3(dir0);
	dFloat w1 = omega1.DotProduct3(dir1);

	dFloat relOmega = w0 + w1;
	dFloat invTimestep = (timestep > 0.0f) ? 1.0f / timestep : 1.0f;
	dFloat relAccel = -0.5f * relOmega * invTimestep;
	NewtonUserJointAddGeneralRow(m_joint, jacobian0, jacobian1);
	NewtonUserJointSetRowAcceleration(m_joint, relAccel);
}

void dAxelJoint::Load(dCustomJointSaveLoad* const fileLoader)
{
	const char* token = fileLoader->NextToken();
	dAssert(!strcmp(token, "parentReference:"));
	int parentIndex = fileLoader->LoadInt();
	m_parentReference = fileLoader->FindBodyId(parentIndex);

	token = fileLoader->NextToken();
	dAssert(!strcmp(token, "pintOnParentReference:"));
	m_pintOnReference = fileLoader->LoadVector();
}

void dAxelJoint::Save(dCustomJointSaveLoad* const fileSaver) const
{
	dCustomGear::Save(fileSaver);
	fileSaver->SaveInt("\tparentReference", fileSaver->FindBodyId(m_parentReference));
	fileSaver->SaveVector("\tpintOnParentReference", m_pintOnReference);
}


/*
dFloat dBodyPartTire::GetRPM() const
{
	dVector omega(0.0f); 
	dWheelJoint* const joint = (dWheelJoint*) m_joint;
	NewtonBodyGetOmega(m_body, &omega[0]);
	return joint->m_lateralDir.DotProduct3(omega) * 9.55f;
}


dFloat dBodyPartTire::GetLateralSlip () const
{
	return m_lateralSlip;
}

dFloat dBodyPartTire::GetLongitudinalSlip () const
{
	return m_longitudinalSlip;
}

void dBodyPartTire::Save(dCustomJointSaveLoad* const fileSaver) const
{
	fileSaver->SaveInt("tire", m_index);
	fileSaver->SaveVector("\tlocation", m_data.m_location);
	fileSaver->SaveFloat("\tmass", m_data.m_mass);
	fileSaver->SaveFloat("\tradio", m_data.m_radio);
	fileSaver->SaveFloat("\twidth", m_data.m_width);
	fileSaver->SaveFloat("\tpivotOffset", m_data.m_pivotOffset);
	fileSaver->SaveFloat("\tmaxSteeringAngle", m_data.m_maxSteeringAngle);
	fileSaver->SaveFloat("\tdampingRatio", m_data.m_dampingRatio);
	fileSaver->SaveFloat("\tspringStrength", m_data.m_springStrength);
	fileSaver->SaveFloat("\tsuspensionLength", m_data.m_suspensionLength);
	fileSaver->SaveFloat("\tlateralStiffness", m_data.m_lateralStiffness);
	fileSaver->SaveFloat("\tlongitudialStiffness", m_data.m_longitudialStiffness);
	fileSaver->SaveFloat("\taligningMomentTrail", m_data.m_aligningMomentTrail);
	fileSaver->SaveInt("\thasFender", m_data.m_hasFender);
	fileSaver->SaveInt("\tsuspentionType", m_data.m_suspentionType);
}
*/


// Using brush tire model explained by Giancarlo Genta in his book, adapted to calculate friction coefficient instead tire forces
void dTireFrictionModel::CalculateTireFrictionCoefficents(
	const dWheelJoint* const tire, const NewtonBody* const otherBody, const NewtonMaterial* const material,
	dFloat longitudinalSlip, dFloat lateralSlip, dFloat longitudinalStiffness, dFloat lateralStiffness,
	dFloat& longitudinalFrictionCoef, dFloat& lateralFrictionCoef, dFloat& aligningTorqueCoef) const
{
	dAssert(lateralSlip >= 0.0f);
	//	dAssert (longitudinalSlip >= 0.0f);
	dAssert(lateralStiffness >= 0.0f);
	dAssert(longitudinalStiffness >= 0.0f);
	dFloat den = 1.0f / (1.0f + longitudinalSlip);

	lateralSlip *= den;
	longitudinalSlip *= den;

	dFloat phy_y = dAbs(lateralStiffness * lateralSlip * 4.0f);
	dFloat phy_x = dAbs(longitudinalStiffness * longitudinalSlip * 4.0f);

	dFloat gamma = dMax(dSqrt(phy_x * phy_x + phy_y * phy_y), dFloat(0.1f));
	dFloat fritionCoeficicent = dClamp(GetFrictionCoefficient(material, tire->GetBody0(), otherBody), dFloat(0.0f), dFloat(1.0f));

	dFloat normalTireLoad = 1.0f * fritionCoeficicent;
	dFloat phyMax = 3.0f * normalTireLoad + 1.0e-3f;
	dFloat F = (gamma <= phyMax) ? (gamma * (1.0f - gamma / phyMax + gamma * gamma / (3.0f * phyMax * phyMax))) : normalTireLoad;

	dFloat fraction = F / gamma;
	dAssert(fraction > 0.0f);
	lateralFrictionCoef = phy_y * fraction;
	longitudinalFrictionCoef = phy_x * fraction;

	dAssert(lateralFrictionCoef >= 0.0f);
	dAssert(lateralFrictionCoef <= 1.1f);
	dAssert(longitudinalFrictionCoef >= 0.0f);
	dAssert(longitudinalFrictionCoef <= 1.1f);

	aligningTorqueCoef = 0.0f;
}

/*
void dBodyPartDifferential::SetTrackMode(bool mode, dFloat trackTurnSpeed)
{
	dDifferentialJoint* const joint = (dDifferentialJoint*)m_joint;
	joint->m_isTractionDifferential = mode;
	m_differentialSpeed = trackTurnSpeed;
}
*/

dEngineController::dEngineController(dCustomVehicleController* const controller, const dEngineInfo& info, dDifferentialJoint* const differential, dWheelJoint* const crownGearCalculator)
	:dVehicleController(controller)
	,m_info(info)
	,m_infoCopy(info)
	,m_controller(controller)
	,m_crownGearCalculator(crownGearCalculator)
	,m_differential(differential)
	,m_gearBoxJoint(NULL)
	,m_clutchParam(1.0f)
	,m_gearTimer(0)
	,m_currentGear(D_VEHICLE_NEUTRAL_GEAR)
	,m_ignitionKey(false)
	,m_automaticTransmissionMode(true)
{
	dMatrix chassisMatrix;
	dMatrix differentialMatrix;

	NewtonBody* const chassisBody = controller->GetBody();
	NewtonBodyGetMatrix (chassisBody, &chassisMatrix[0][0]);
	
	NewtonBody* const engineBody = controller->m_engine->GetBody0();
	dAssert (engineBody == controller->m_engine->GetEngineBody());

	NewtonBody* const differentialBody = m_differential->GetBody0();
	dAssert (differentialBody == m_differential->GetDifferentialBody());
	NewtonBodyGetMatrix (engineBody, &differentialMatrix[0][0]);
	m_gearBoxJoint = new dGearBoxJoint(chassisMatrix.m_right, differentialBody, engineBody, m_info.m_clutchFrictionTorque);
	SetInfo(info);
}

dEngineController::~dEngineController()
{
}

dEngineInfo dEngineController::GetInfo() const
{
	return m_infoCopy;
}

void dEngineController::SetInfo(const dEngineInfo& info)
{
	m_info = info;
	m_infoCopy = info;
	m_info.m_clutchFrictionTorque = dMax (dFloat(10.0f), dAbs (m_info.m_clutchFrictionTorque));
	m_infoCopy.m_clutchFrictionTorque = m_info.m_clutchFrictionTorque;
	InitEngineTorqueCurve();

	dAssert(info.m_gearsCount < (int(sizeof (m_info.m_gearRatios) / sizeof (m_info.m_gearRatios[0])) - D_VEHICLE_FIRST_GEAR));
	m_info.m_gearsCount = info.m_gearsCount + D_VEHICLE_FIRST_GEAR;

	m_info.m_gearRatios[D_VEHICLE_NEUTRAL_GEAR] = 0.0f;
	m_info.m_gearRatios[D_VEHICLE_REVERSE_GEAR] = -dAbs(info.m_reverseGearRatio);
	for (int i = 0; i < (m_info.m_gearsCount - D_VEHICLE_FIRST_GEAR); i++) {
		m_info.m_gearRatios[i + D_VEHICLE_FIRST_GEAR] = dAbs(info.m_gearRatios[i]);
	}

	for (dList<dWheelJoint*>::dListNode* tireNode = m_controller->m_tireList.GetFirst(); tireNode; tireNode = tireNode->GetNext()) {
		dWheelJoint* const tire = tireNode->GetInfo();
		dFloat angle = (1.0f / 30.0f) * (0.277778f) * info.m_vehicleTopSpeed / tire->m_radio;
		NewtonBodySetMaxRotationPerStep(tire->GetTireBody(), angle);
	}
	m_controller->SetAerodynamicsDownforceCoefficient (info.m_aerodynamicDownforceFactor, info.m_aerodynamicDownForceSurfaceCoeficident, info.m_aerodynamicDownforceFactorAtTopSpeed);
}

bool dEngineController::GetDifferentialLock() const
{
	return m_info.m_differentialLock ? true : false;
}

void dEngineController::SetDifferentialLock(bool lock)
{
	m_info.m_differentialLock = lock ? 1 : 0;
}

dFloat dEngineController::GetTopGear() const
{
	return m_info.m_gearRatios[m_info.m_gearsCount - 1];
}

void dEngineController::InitEngineTorqueCurve()
{
	m_info.ConvertToMetricSystem();

	dAssert(m_info.m_vehicleTopSpeed >= 0.0f);
	dAssert(m_info.m_vehicleTopSpeed < 100.0f);

	m_info.m_crownGearRatio = 1.0f;
	dWheelJoint* const tire = m_crownGearCalculator;
	dAssert(tire);

	// drive train geometrical relations
	// G0 = m_differentialGearRatio
	// G1 = m_transmissionGearRatio
	// s = topSpeedMPS
	// r = tireRadio
	// wt = rpsAtTire
	// we = rpsAtPickPower
	// we = G1 * G0 * wt;
	// wt = e / r
	// we = G0 * G1 * s / r
	// G0 = r * we / (G1 * s)
	// using the top gear and the optimal engine torque for the calculations
	dFloat topGearRatio = GetTopGear();
	dFloat tireRadio = tire->m_radio;
	m_info.m_crownGearRatio = tireRadio * m_info.m_rpmAtPeakHorsePower / (m_info.m_vehicleTopSpeed * topGearRatio);

	// bake crown gear with the engine power curve
	//m_info.m_idleFriction *= m_info.m_crownGearRatio;
	m_info.m_idleTorque *= m_info.m_crownGearRatio;
	m_info.m_peakTorque *= m_info.m_crownGearRatio;
	m_info.m_peakPowerTorque *= m_info.m_crownGearRatio;

	m_info.m_rpmAtIdleTorque /= m_info.m_crownGearRatio;
	m_info.m_rpmAtPeakTorque /= m_info.m_crownGearRatio;
	m_info.m_rpmAtPeakHorsePower /= m_info.m_crownGearRatio;
	m_info.m_rpmAtRedLine /= m_info.m_crownGearRatio;

	dFloat rpmStep = m_info.m_rpmAtIdleTorque;
	m_info.m_viscousDrag = m_info.m_idleTorque * D_VEHICLE_ENGINE_IDLE_GAS_VALVE / (rpmStep * rpmStep);

	m_info.m_torqueCurve[0] = dEngineTorqueNode (0.0f, m_info.m_idleTorque);
	m_info.m_torqueCurve[1] = dEngineTorqueNode (m_info.m_rpmAtIdleTorque, m_info.m_idleTorque);
	m_info.m_torqueCurve[2] = dEngineTorqueNode (m_info.m_rpmAtPeakTorque, m_info.m_peakTorque);
	m_info.m_torqueCurve[3] = dEngineTorqueNode (m_info.m_rpmAtPeakHorsePower, m_info.m_peakPowerTorque);
	m_info.m_torqueCurve[4] = dEngineTorqueNode (m_info.m_rpmAtRedLine, m_info.m_idleTorque * D_VEHICLE_ENGINE_IDLE_GAS_VALVE);
	m_info.m_torqueCurve[5] = dEngineTorqueNode (m_info.m_rpmAtRedLine, m_info.m_idleTorque * D_VEHICLE_ENGINE_IDLE_GAS_VALVE);
}

void dEngineController::PlotEngineCurve() const
{
#ifdef D_PLOT_ENGINE_CURVE 
	dFloat rpm0 = m_torqueRPMCurve.m_nodes[0].m_param;
	dFloat rpm1 = m_torqueRPMCurve.m_nodes[m_torqueRPMCurve.m_count - 1].m_param;
	int steps = 40;
	dFloat omegaStep = (rpm1 - rpm0) / steps;
	dTrace(("rpm\ttorque\tpower\n"));
	for (int i = 0; i < steps; i++) {
		dFloat r = rpm0 + omegaStep * i;
		dFloat torque = m_torqueRPMCurve.GetValue(r);
		dFloat power = r * torque;
		const dFloat horsePowerToWatts = 735.5f;
		const dFloat rpmToRadiansPerSecunds = 0.105f;
		const dFloat poundFootToNewtonMeters = 1.356f;
		dTrace(("%6.2f\t%6.2f\t%6.2f\n", r / 0.105f, torque / 1.356f, power / 735.5f));
	}
#endif
}

dFloat dEngineController::GetGearRatio () const
{
	return m_info.m_crownGearRatio * m_info.m_gearRatios[m_currentGear];
}

void dEngineController::UpdateAutomaticGearBox(dFloat timestep, dFloat omega)
{
m_info.m_gearsCount = 4;

	m_gearTimer--;
	if (m_gearTimer < 0) {
		switch (m_currentGear) 
		{
			case D_VEHICLE_NEUTRAL_GEAR:
			{
			   SetGear(D_VEHICLE_NEUTRAL_GEAR);
			   break;
			}

			case D_VEHICLE_REVERSE_GEAR:
			{
				SetGear(D_VEHICLE_REVERSE_GEAR);
				break;
			}

			default:
			{
			   if (omega > m_info.m_rpmAtPeakHorsePower) {
					if (m_currentGear < (m_info.m_gearsCount - 1)) {
						SetGear(m_currentGear + 1);
					}
				} else if (omega < m_info.m_rpmAtPeakTorque) {
					if (m_currentGear > D_VEHICLE_FIRST_GEAR) {
						SetGear(m_currentGear - 1);
					}
				}
			}
		}
	}
}

void dEngineController::ApplyTorque(dFloat torqueMag)
{
	dMatrix matrix;
	dEngineJoint* const engineJoint = m_controller->m_engine;
	NewtonBody* const engine = engineJoint->GetBody0();
	NewtonBody* const chassis = engineJoint->GetBody1();
	dAssert (m_controller->GetBody() == chassis);
	
	NewtonBodyGetMatrix(chassis, &matrix[0][0]);
	dVector torque(matrix.m_right.Scale(torqueMag));
	NewtonBodyAddTorque(engine, &torque[0]);

//	m_controller->m_engine->ApplyTorque(torque);
}

dFloat dEngineController::IntepolateTorque(dFloat rpm) const
{
	dAssert (rpm >= -0.1f);
	const int maxIndex = sizeof (m_info.m_torqueCurve) / sizeof (m_info.m_torqueCurve[0]) - 1;
	rpm = dClamp (rpm, dFloat (0.0f), m_info.m_torqueCurve[maxIndex - 1].m_rpm);

	for(int i = 1; i < maxIndex; i ++) {
		if (m_info.m_torqueCurve[i].m_rpm >= rpm) {
			dFloat rpm0 = m_info.m_torqueCurve[i - 1].m_rpm;
			dFloat rpm1 = m_info.m_torqueCurve[i].m_rpm;

			dFloat torque0 = m_info.m_torqueCurve[i - 1].m_torque;
			dFloat torque1 = m_info.m_torqueCurve[i].m_torque;
			dFloat torque = torque0 + (rpm - rpm0) * (torque1 - torque0) / (rpm1 - rpm0);
			return torque;
		}
	}

	return m_info.m_torqueCurve[maxIndex - 1].m_rpm;
}

void dEngineController::Update(dFloat timestep)
{
	dFloat omega = GetRadiansPerSecond();
	if (m_automaticTransmissionMode) {
		UpdateAutomaticGearBox (timestep, omega);
	}

	dEngineJoint* const engineJoint = m_controller->m_engine;
	if (m_ignitionKey) {
		if (m_param < D_VEHICLE_ENGINE_IDLE_GAS_VALVE) {
			// handle idle gas valve 
			dFloat gasEngineTorque = m_info.m_idleTorque * D_VEHICLE_ENGINE_IDLE_GAS_VALVE;
			if (omega < m_info.m_rpmAtIdleTorque) {
				engineJoint->m_minFriction = - m_info.m_viscousDrag * omega * omega;
			} else {
				gasEngineTorque = 0.0f;
				engineJoint->m_minFriction = - IntepolateTorque(omega);
			}
			ApplyTorque(gasEngineTorque);

		} else {
			dFloat rpmIdleStep = dMin(omega, m_info.m_rpmAtIdleTorque);
			dFloat engineTorque = m_info.m_peakTorque * m_param - (IntepolateTorque(omega) - m_info.m_viscousDrag * rpmIdleStep * rpmIdleStep);
			engineJoint->m_minFriction = -engineTorque;
			ApplyTorque(m_info.m_peakTorque * m_param);
		}
	} else {
		ApplyTorque(0.0f);
		engineJoint->m_minFriction = -m_info.m_peakTorque;
	}

	engineJoint->m_maxFriction = m_info.m_peakTorque;
	engineJoint->SetRedLineRPM(m_info.m_rpmAtRedLine);

}

bool dEngineController::GetTransmissionMode() const
{
	return m_automaticTransmissionMode;
}

void dEngineController::SetIgnition(bool key)
{
	m_ignitionKey = key;
}

bool dEngineController::GetIgnition() const
{
	return m_ignitionKey;
}


void dEngineController::SetTransmissionMode(bool mode)
{
	m_automaticTransmissionMode = mode;
}

void dEngineController::SetClutchParam (dFloat cluthParam)
{
	m_clutchParam = dClamp (cluthParam, dFloat(0.0f), dFloat(1.0f));
	if (m_gearBoxJoint) {
		m_gearBoxJoint->SetFritionTorque(m_clutchParam);
	}
}


int dEngineController::GetGear() const
{
	return m_currentGear;
}

void dEngineController::SetGear(int gear)
{
	m_gearTimer = 30;
	m_currentGear = dClamp(gear, 0, m_info.m_gearsCount);
	m_gearBoxJoint->SetGearRatio(m_info.m_gearRatios[m_currentGear] * m_info.m_gearRatiosSign);
}

int dEngineController::GetNeutralGear() const
{
	return D_VEHICLE_NEUTRAL_GEAR;
}

int dEngineController::GetReverseGear() const
{
	return D_VEHICLE_REVERSE_GEAR;
}

int dEngineController::GetFirstGear() const
{
	return D_VEHICLE_FIRST_GEAR;
}

int dEngineController::GetLastGear() const
{
	return m_info.m_gearsCount - 1;
}

dFloat dEngineController::GetRadiansPerSecond() const
{
	dMatrix matrix;
	dVector omega(0.0f);

//	NewtonBody* const chassis = m_controller->GetBody();
//	NewtonBody* const engine = m_controller->m_engine->GetBody();

	dEngineJoint* const engineJoint = m_controller->m_engine;
	NewtonBody* const engine = engineJoint->GetBody0();
	NewtonBody* const chassis = engineJoint->GetBody1();
	dAssert(m_controller->GetBody() == chassis);
	dAssert(engineJoint->GetEngineBody() == engine);

	NewtonBodyGetMatrix(chassis, &matrix[0][0]);
	NewtonBodyGetOmega(engine, &omega[0]);
	return omega.DotProduct3(matrix.m_right);
}

dFloat dEngineController::GetRPM() const
{
	return m_info.m_crownGearRatio * GetRadiansPerSecond() * 9.55f;
}

dFloat dEngineController::GetIdleRPM() const
{
	return m_info.m_crownGearRatio * m_info.m_rpmAtIdleTorque * 9.55f;
}

dFloat dEngineController::GetRedLineRPM() const
{
	return m_info.m_crownGearRatio * m_info.m_rpmAtRedLine * 9.55f;
}

dFloat dEngineController::GetSpeed() const
{
	dMatrix matrix;
	dVector veloc(0.0f);
	NewtonBody* const chassis = m_controller->GetBody();

	NewtonBodyGetMatrix(chassis, &matrix[0][0]);
	NewtonBodyGetVelocity(chassis, &veloc[0]);

//	dVector pin (matrix.RotateVector (m_controller->m_localFrame.m_front));
	dVector pin (matrix.m_front);
	return pin.DotProduct3(veloc);
}

dFloat dEngineController::GetTopSpeed() const
{
	return m_info.m_vehicleTopSpeed;
}

dSteeringController::dSteeringController (dCustomVehicleController* const controller)
	:dVehicleController(controller)
	,m_isSleeping(false)
{
}

void dSteeringController::Update(dFloat timestep)
{
	m_isSleeping = true;
	for (dList<dWheelJoint*>::dListNode* node = m_tires.GetFirst(); node; node = node->GetNext()) {
		dWheelJoint* const tire = node->GetInfo();

		tire->m_steerAngle1 = -m_param * tire->m_maxSteeringAngle;
		if (tire->m_steerAngle0 < tire->m_steerAngle1) {
			tire->m_steerAngle0 += tire->m_steerRate * timestep;
			if (tire->m_steerAngle0 > tire->m_steerAngle1) {
				tire->m_steerAngle0 = tire->m_steerAngle1;
			}
		} else if (tire->m_steerAngle0 > tire->m_steerAngle1) {
			tire->m_steerAngle0 -= tire->m_steerRate * timestep;
			if (tire->m_steerAngle0 < tire->m_steerAngle1) {
				tire->m_steerAngle0 = tire->m_steerAngle1;
			}
		}
		m_isSleeping &= dAbs(tire->m_steerAngle1 - tire->m_steerAngle0) < 1.0e-4f;
	}
}

void dSteeringController::AddTire (dWheelJoint* const tire)
{
	m_tires.Append(tire);
}

dTractionSteeringController::dTractionSteeringController(dCustomVehicleController* const controller, dDifferentialJoint* const differential)
	:dSteeringController(controller)
	,m_differential(differential)
{
	dAssert(0);
}

void dTractionSteeringController::Update(dFloat timestep)
{
	dAssert (0);
/*
	dDifferentialJoint* const diffJoint = (dDifferentialJoint*) m_differential->GetJoint();
	diffJoint->m_turnSpeed = -m_param * m_differential->m_differentialSpeed;
*/
}


dBrakeController::dBrakeController(dCustomVehicleController* const controller, dFloat maxBrakeTorque)
	:dVehicleController(controller)
	,m_maxTorque(maxBrakeTorque)
{
}

void dBrakeController::AddTire(dWheelJoint* const tire)
{
	m_tires.Append(tire);
}

void dBrakeController::Update(dFloat timestep)
{
	dFloat torque = m_maxTorque * m_param;
   	for (dList<dWheelJoint*>::dListNode* node = m_tires.GetFirst(); node; node = node->GetNext()) {
		dWheelJoint* const tire = node->GetInfo();
		//tire.SetBrakeTorque (torque);
		tire->m_brakeTorque = dMax(torque, tire->m_brakeTorque);
	}
}

void dCustomVehicleControllerManager::DrawSchematic (const dCustomVehicleController* const controller, dFloat scale) const
{
	controller->DrawSchematic(scale);
}

void dCustomVehicleControllerManager::DrawSchematicCallback (const dCustomVehicleController* const controller, const char* const partName, dFloat value, int pointCount, const dVector* const lines) const
{
}

void dCustomVehicleController::DrawSchematic(dFloat scale) const
{
//	dAssert (0);
/*
	dMatrix projectionMatrix(dGetIdentityMatrix());
	projectionMatrix[0][0] = scale;
	projectionMatrix[1][1] = 0.0f;
	projectionMatrix[2][1] = -scale;
	projectionMatrix[2][2] = 0.0f;
	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();

	dMatrix matrix0;
	dVector com(0.0f);
	dFloat arrayPtr[32][4];
	dVector* const array = (dVector*)arrayPtr;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;
	NewtonBody* const chassisBody = m_chassis.GetBody();

	dFloat velocityScale = 0.125f;

	NewtonBodyGetCentreOfMass(chassisBody, &com[0]);
	NewtonBodyGetMatrix(chassisBody, &matrix0[0][0]);
	matrix0.m_posit = matrix0.TransformVector(com);

	NewtonBodyGetMass(chassisBody, &mass, &Ixx, &Iyy, &Izz);
	dMatrix chassisMatrix(matrix0);
	dMatrix worldToComMatrix(chassisMatrix.Inverse() * projectionMatrix);
	{
		// draw vehicle chassis
		dVector p0(D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, 0.0f);
		dVector p1(-D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, 0.0f);

		for (dList<dBodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			dBodyPartTire* const tire = &node->GetInfo();
			NewtonBody* const tireBody = tire->GetBody();
			
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			//dMatrix matrix (tire->CalculateSteeringMatrix() * m_chassisState.GetMatrix());
			dVector p(worldToComMatrix.TransformVector(matrix.m_posit));
			p0 = dVector(dMin(p.m_x, p0.m_x), dMin(p.m_y, p0.m_y), dMin(p.m_z, p0.m_z), 1.0f);
			p1 = dVector(dMax(p.m_x, p1.m_x), dMax(p.m_y, p1.m_y), dMax(p.m_z, p1.m_z), 1.0f);
		}

		array[0] = dVector(p0.m_x, p0.m_y, p0.m_z, 1.0f);
		array[1] = dVector(p1.m_x, p0.m_y, p0.m_z, 1.0f);
		array[2] = dVector(p1.m_x, p1.m_y, p0.m_z, 1.0f);
		array[3] = dVector(p0.m_x, p1.m_y, p0.m_z, 1.0f);
		manager->DrawSchematicCallback(this, "chassis", 0, 4, array);
	}

	{
		// draw vehicle tires
		for (dList<dBodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			dBodyPartTire* const tire = &node->GetInfo();
			dFloat width = tire->m_data.m_width * 0.5f;
			dFloat radio = tire->m_data.m_radio;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front.CrossProduct(matrix.m_up);

			array[0] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, radio, 0.0f)));
			array[1] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, -radio, 0.0f)));
			array[2] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, -radio, 0.0f)));
			array[3] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, radio, 0.0f)));
			manager->DrawSchematicCallback(this, "tire", 0, 4, array);
		}
	}

	{
		// draw vehicle velocity
		dVector veloc(0.0f);
		dVector omega(0.0f);

		NewtonBodyGetOmega(chassisBody, &omega[0]);
		NewtonBodyGetVelocity(chassisBody, &veloc[0]);

		dVector localVelocity(chassisMatrix.UnrotateVector(veloc));
		localVelocity.m_y = 0.0f;

		localVelocity = projectionMatrix.RotateVector(localVelocity);
		array[0] = dVector(0.0f);
		array[1] = localVelocity.Scale(velocityScale);
		manager->DrawSchematicCallback(this, "velocity", 0, 2, array);

		dVector localOmega(chassisMatrix.UnrotateVector(omega));
		array[0] = dVector(0.0f);
		array[1] = dVector(0.0f, localOmega.m_y * 10.0f, 0.0f, 0.0f);
		manager->DrawSchematicCallback(this, "omega", 0, 2, array);
	}

	{
		dFloat scale1(2.0f / (mass * 10.0f));
		// draw vehicle forces
		for (dList<dBodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dBodyPartTire* const tire = &node->GetInfo();
			dMatrix matrix;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front.CrossProduct(matrix.m_up);

			dVector origin(worldToComMatrix.TransformVector(matrix.m_posit));

			dVector lateralForce(chassisMatrix.UnrotateVector(GetTireLateralForce(tire)));
			lateralForce = lateralForce.Scale(-scale1);
			lateralForce = projectionMatrix.RotateVector(lateralForce);

			array[0] = origin;
			array[1] = origin + lateralForce;
			manager->DrawSchematicCallback(this, "lateralForce", 0, 2, array);

			dVector longitudinalForce(chassisMatrix.UnrotateVector(GetTireLongitudinalForce(tire)));
			longitudinalForce = longitudinalForce.Scale(-scale1);
			longitudinalForce = projectionMatrix.RotateVector(longitudinalForce);

			array[0] = origin;
			array[1] = origin + longitudinalForce;
			manager->DrawSchematicCallback(this, "longitudinalForce", 0, 2, array);

			dVector veloc(0.0f);
			NewtonBodyGetVelocity(tireBody, &veloc[0]);
			veloc = chassisMatrix.UnrotateVector(veloc);
			veloc.m_y = 0.0f;
			veloc = projectionMatrix.RotateVector(veloc);
			array[0] = origin;
			array[1] = origin + veloc.Scale(velocityScale);
			manager->DrawSchematicCallback(this, "tireVelocity", 0, 2, array);
		}
	}
*/
}

dCustomVehicleControllerManager::dCustomVehicleControllerManager(NewtonWorld* const world, int materialCount, int* const materialsList)
	:dCustomControllerManager<dCustomVehicleController> (world, VEHICLE_PLUGIN_NAME)
	,m_tireMaterial(NewtonMaterialCreateGroupID(world))
{
	// create the normalized size tire shape
	dMatrix alignment (dYawMatrix(90.0f * 3.141592f / 180.0f));
	m_tireShapeTemplate = NewtonCreateChamferCylinder(world, 0.5f, 1.0f, 0, &alignment[0][0]);
	m_tireShapeTemplateData = NewtonCollisionDataPointer(m_tireShapeTemplate);

	// create a tire material and associate with the material the vehicle new to collide 
	for (int i = 0; i < materialCount; i ++) {
		NewtonMaterialSetCallbackUserData (world, m_tireMaterial, materialsList[i], this);
		if (m_tireMaterial != materialsList[i]) {
			NewtonMaterialSetContactGenerationCallback (world, m_tireMaterial, materialsList[i], OnContactGeneration);
		}
		NewtonMaterialSetCollisionCallback(world, m_tireMaterial, materialsList[i], OnTireAABBOverlap, OnTireContactsProcess);
	}
}

dCustomVehicleControllerManager::~dCustomVehicleControllerManager()
{
	NewtonDestroyCollision(m_tireShapeTemplate);
}

void dCustomVehicleControllerManager::DestroyController(dCustomVehicleController* const controller)
{
	controller->Cleanup();
	dCustomControllerManager<dCustomVehicleController>::DestroyController(controller);
}

int dCustomVehicleControllerManager::GetTireMaterial() const
{
	return m_tireMaterial;
}

dCustomVehicleController* dCustomVehicleControllerManager::CreateVehicle(NewtonBody* const body, const dMatrix& vehicleFrame, NewtonApplyForceAndTorque forceAndTorque, dFloat gravityMag)
{
	dCustomVehicleController* const controller = CreateController();
	controller->Init (body, forceAndTorque, gravityMag);
	return controller;
}

dCustomVehicleController* dCustomVehicleControllerManager::CreateVehicle(NewtonCollision* const chassisShape, const dMatrix& vehicleFrame, dFloat mass, NewtonApplyForceAndTorque forceAndTorque, dFloat gravityMag)
{
	dCustomVehicleController* const controller = CreateController();
	controller->Init(chassisShape, mass, forceAndTorque, gravityMag);
	return controller;
}


class dCustomVehicleControllerManager::dTireFilter: public dCustomControllerConvexCastPreFilter
{
	public:
	dTireFilter(const dWheelJoint* const tire, const dCustomVehicleController* const controller)
		:dCustomControllerConvexCastPreFilter(tire->GetTireBody())
		, m_controller(controller)
		, m_tire(tire)
	{
	}

	unsigned Prefilter(const NewtonBody* const body, const NewtonCollision* const myCollision)
	{
		dAssert(body != m_me);
		for (int i = 0; i < m_tire->m_collidingCount; i++) {
			if (m_tire->m_contactInfo[i].m_hitBody == body) {
				return 0;
			}
		}

		for (dList<NewtonBody*>::dListNode* node = m_controller->m_bodyList.GetFirst(); node; node = node->GetNext()) {
			if (node->GetInfo() == body) {
				return 0;
			}
		}

		return (body != m_controller->GetBody()) ? 1 : 0;
	}

	const dWheelJoint* m_tire;
	const dCustomVehicleController* m_controller;
};


void dCustomVehicleController::Init(NewtonCollision* const chassisShape, dFloat mass, NewtonApplyForceAndTorque forceAndTorque, dFloat gravityMag)
{
	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = manager->GetWorld();

	// create a body and an call the low level init function
	dMatrix locationMatrix(dGetIdentityMatrix());
	NewtonBody* const body = NewtonCreateDynamicBody(world, chassisShape, &locationMatrix[0][0]);

	// set vehicle mass, inertia and center of mass
	NewtonBodySetMassProperties(body, mass, chassisShape);

	// initialize 
	Init(body, forceAndTorque, gravityMag);
}

void dCustomVehicleController::Init(NewtonBody* const body, NewtonApplyForceAndTorque forceAndTorque, dFloat gravityMag)
{
	m_body = body;
	m_speed = 0.0f;
	m_sideSlip = 0.0f;
	m_prevSideSlip = 0.0f;
	m_finalized = false;
	m_gravityMag = gravityMag;
	m_weightDistribution = 0.5f;
	m_aerodynamicsDownForce0 = 0.0f;
	m_aerodynamicsDownForce1 = 0.0f;
	m_aerodynamicsDownSpeedCutOff = 0.0f;
	m_aerodynamicsDownForceCoefficient = 0.0f;

	m_forceAndTorqueCallback = forceAndTorque;

	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = manager->GetWorld();

	// set linear and angular drag to zero
	dVector drag(0.0f);
	NewtonBodySetLinearDamping(m_body, 0);
	NewtonBodySetAngularDamping(m_body, &drag[0]);

	// set the standard force and torque call back
	NewtonBodySetForceAndTorqueCallback(body, m_forceAndTorqueCallback);

	m_contactFilter = new dTireFrictionModel(this);

	m_engine = NULL;
	m_brakesControl = NULL;
	m_engineControl = NULL;
	m_handBrakesControl = NULL;
	m_steeringControl = NULL;
	
	m_collisionAggregate = NewtonCollisionAggregateCreate(world);
	NewtonCollisionAggregateSetSelfCollision (m_collisionAggregate, 0);
	NewtonCollisionAggregateAddBody (m_collisionAggregate, m_body);

//	m_chassis.Init(this, userData);
//	m_bodyPartsList.Append(&m_chassis);
	m_bodyList.Append(m_body);

	SetAerodynamicsDownforceCoefficient(0.5f, 0.4f, 1.0f);

#ifdef D_PLOT_ENGINE_CURVE 
	file_xxx = fopen("vehiceLog.csv", "wb");
	fprintf (file_xxx, "eng_rpm, eng_torque, eng_nominalTorque,\n");
#endif
}

void dCustomVehicleController::Cleanup()
{
	if (m_engine) {
		delete m_engine;
	}

	SetBrakes(NULL);
	SetEngine(NULL);
	SetSteering(NULL);
	SetHandBrakes(NULL);
	SetContactFilter(NULL);
}


dEngineController* dCustomVehicleController::GetEngine() const
{
	return m_engineControl;
}


dSteeringController* dCustomVehicleController::GetSteering() const
{
	return m_steeringControl;
}

dBrakeController* dCustomVehicleController::GetBrakes() const
{
	return m_brakesControl;
}

dBrakeController* dCustomVehicleController::GetHandBrakes() const
{
	return m_handBrakesControl;
}

void dCustomVehicleController::SetEngine(dEngineController* const engineControl)
{
	if (m_engineControl) {
		delete m_engineControl;
	}
	m_engineControl = engineControl;
}


void dCustomVehicleController::SetHandBrakes(dBrakeController* const handBrakes)
{
	if (m_handBrakesControl) {
		delete m_handBrakesControl;
	}
	m_handBrakesControl = handBrakes;
}

void dCustomVehicleController::SetBrakes(dBrakeController* const brakes)
{
	if (m_brakesControl) {
		delete m_brakesControl;
	}
	m_brakesControl = brakes;
}


void dCustomVehicleController::SetSteering(dSteeringController* const steering)
{
	if (m_steeringControl) {
		delete m_steeringControl;
	}
	m_steeringControl = steering;
}

void dCustomVehicleController::SetContactFilter(dTireFrictionModel* const filter)
{
	if (m_contactFilter) {
		delete m_contactFilter;
	}
	m_contactFilter = filter;
}

dList<dWheelJoint*>::dListNode* dCustomVehicleController::GetFirstTire() const
{
	return m_tireList.GetFirst();
}

dList<dWheelJoint*>::dListNode* dCustomVehicleController::GetNextTire(dList<dWheelJoint*>::dListNode* const tireNode) const
{
	return tireNode->GetNext();
}

dList<dDifferentialJoint*>::dListNode* dCustomVehicleController::GetFirstDifferential() const
{
	return m_differentialList.GetFirst();
}

dList<dDifferentialJoint*>::dListNode* dCustomVehicleController::GetNextDifferential(dList<dDifferentialJoint*>::dListNode* const differentialNode) const
{
	return differentialNode->GetNext();
}

dList<NewtonBody*>::dListNode* dCustomVehicleController::GetFirstBodyPart() const
{
	return m_bodyList.GetFirst();
}

dList<NewtonBody*>::dListNode* dCustomVehicleController::GetNextBodyPart(dList<NewtonBody*>::dListNode* const part) const
{
	return part->GetNext();
}


void dCustomVehicleController::SetCenterOfGravity(const dVector& comRelativeToGeomtriCenter)
{
	NewtonBodySetCentreOfMass(m_body, &comRelativeToGeomtriCenter[0]);
}

dFloat dCustomVehicleController::GetWeightDistribution() const
{
	return m_weightDistribution;
}

void dCustomVehicleController::SetWeightDistribution(dFloat weightDistribution)
{
	dFloat totalMass;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;

	NewtonBodyGetMass (m_body, &totalMass, &Ixx, &Iyy, &Izz);

	m_totalMass = totalMass;
	m_weightDistribution = dClamp (weightDistribution, dFloat(0.0f), dFloat(1.0f));
	if (m_finalized) {
		dFloat factor = m_weightDistribution - 0.5f;

		dMatrix matrix;
		dMatrix tireMatrix;
		dVector origin(0.0f);
		dVector totalMassOrigin (0.0f);
		
		NewtonBodyGetMatrix(m_body, &matrix[0][0]);
		NewtonBodyGetCentreOfMass(m_body, &origin[0]);
		matrix.m_posit = matrix.TransformVector(origin);
		totalMassOrigin = origin.Scale (totalMass);

		NewtonCollision* const collision = NewtonBodyGetCollision(m_body);
		dVector support(0.0f);
		dVector localDir(1.0f, 0.0f, 0.0f, 0.0f);
		NewtonCollisionSupportVertex(collision, &localDir[0], &support[0]);
		dFloat minX = support[0];

		localDir.m_x = -1.0f;
		NewtonCollisionSupportVertex(collision, &localDir[0], &support[0]);
		dFloat maxX = support[0];

		matrix = matrix.Inverse();
		if (m_engine) {
			dMatrix engineMatrixMatrix;
			dFloat mass;
			NewtonBody* const engineBody = m_engine->GetBody0();
			NewtonBodyGetMass (engineBody, &mass, &Ixx, &Iyy, &Izz);
			NewtonBodyGetMatrix(engineBody, &engineMatrixMatrix[0][0]);
			totalMass += mass;
			engineMatrixMatrix = engineMatrixMatrix * matrix;
			totalMassOrigin += engineMatrixMatrix.m_posit.Scale (mass);
		}

		for (dList<dWheelJoint*>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dFloat mass;
			dWheelJoint* const tire = node->GetInfo();
			NewtonBody* const tireBody = tire->GetBody0();
			NewtonBodyGetMatrix(tireBody, &tireMatrix[0][0]);
			NewtonBodyGetMass (tireBody, &mass, &Ixx, &Iyy, &Izz);

			totalMass += mass;
			tireMatrix = tireMatrix * matrix;
			totalMassOrigin += tireMatrix.m_posit.Scale (mass);
			minX = dMin (minX, tireMatrix.m_posit.m_x); 
			maxX = dMax (maxX, tireMatrix.m_posit.m_x); 
		}
		origin = totalMassOrigin.Scale (1.0f / totalMass);

		dVector vehCom (0.0f);
		NewtonBodyGetCentreOfMass(m_body, &vehCom[0]);
		vehCom.m_x = origin.m_x + (maxX - minX) * factor;
		vehCom.m_z = origin.m_z;
		NewtonBodySetCentreOfMass(m_body, &vehCom[0]);
		m_totalMass = totalMass;
	}
}

void dCustomVehicleController::Finalize()
{
	m_finalized = true;
	SetWeightDistribution (m_weightDistribution);
}

bool dCustomVehicleController::ControlStateChanged() const
{
	bool inputChanged = (m_steeringControl && m_steeringControl->ParamChanged());
	inputChanged = inputChanged || (m_steeringControl && !m_steeringControl->m_isSleeping);
	inputChanged = inputChanged || (m_engineControl && m_engineControl ->ParamChanged());
	inputChanged = inputChanged || (m_brakesControl && m_brakesControl->ParamChanged());
	inputChanged = inputChanged || (m_handBrakesControl && m_handBrakesControl->ParamChanged());
//	inputChanged = inputChanged || m_hasNewContact;
	return inputChanged;
}

dWheelJoint* dCustomVehicleController::AddTire(const dTireInfo& tireInfo)
{
	dVector drag(0.0f);

	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = ((dCustomVehicleControllerManager*)manager)->GetWorld();
	NewtonCollisionSetScale(manager->m_tireShapeTemplate, tireInfo.m_width, tireInfo.m_radio, tireInfo.m_radio);

	dMatrix locationInGlobalSpase;
	NewtonBodyGetMatrix(m_body, &locationInGlobalSpase[0][0]);
	locationInGlobalSpase.m_posit = locationInGlobalSpase.TransformVector(tireInfo.m_location);
	dAssert (locationInGlobalSpase.m_posit.m_w == 1.0f);

	// create the rigid body that will make this bone
	NewtonBody* const tireBody = NewtonCreateDynamicBody(world, manager->m_tireShapeTemplate, &locationInGlobalSpase[0][0]);
	m_bodyList.Append(tireBody);

	NewtonCollision* const collision = NewtonBodyGetCollision(tireBody);
	NewtonBodySetMaterialGroupID(tireBody, manager->GetTireMaterial());

	NewtonBodySetLinearDamping(tireBody, 0.0f);
	NewtonBodySetAngularDamping(tireBody, &drag[0]);
	NewtonBodySetMaxRotationPerStep(tireBody, 3.141692f);

	// set the standard force and torque call back
	NewtonBodySetForceAndTorqueCallback(tireBody, m_forceAndTorqueCallback);

	// tire are highly non linear, sung spherical inertia matrix make the calculation more accurate 
	dFloat inertia = 2.0f * tireInfo.m_mass * tireInfo.m_radio * tireInfo.m_radio / 5.0f;
	NewtonBodySetMassMatrix(tireBody, tireInfo.m_mass, inertia, inertia, inertia);

	dMatrix matrix (dYawMatrix(-90.0f * 3.141592f / 180.0f) * locationInGlobalSpase);
	matrix.m_posit += matrix.m_front.Scale(tireInfo.m_pivotOffset);
	dWheelJoint* const joint = new dWheelJoint(matrix, tireBody, m_body, this, tireInfo);
//	joint->m_index = m_tireList.GetCount();
	m_tireList.Append(joint);

	NewtonCollisionSetUserData1(collision, joint);
	NewtonCollisionAggregateAddBody (m_collisionAggregate, tireBody);
	return joint;
}

dDifferentialJoint* dCustomVehicleController::AddDifferential(dWheelJoint* const leftTire, dWheelJoint* const rightTire)
{
	dMatrix matrix;
	dVector origin;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;

//	dList<dBodyPartDifferential>::dListNode* const differentialNode = m_differentialList.Append();
//	dBodyPartDifferential* const differential = &differentialNode->GetInfo();
//	differential->Init(this, mass, Ixx);
//	m_parent = &controller->m_chassis;
//	m_controller = controller;
//	NewtonBody* const chassisBody = m_controller->GetBody();
//	NewtonWorld* const world = ((dCustomVehicleControllerManager*)m_controller->GetManager())->GetWorld();

	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = ((dCustomVehicleControllerManager*)manager)->GetWorld();

	// get engine location (place at the body center of mass)
	NewtonBodyGetCentreOfMass(m_body, &origin[0]);
	NewtonBodyGetMatrix(m_body, &matrix[0][0]);
	matrix.m_posit = matrix.TransformVector(origin);

	NewtonCollision* const collision = NewtonCreateSphere(world, 0.1f, 0, NULL);
	//NewtonCollision* const collision = NewtonCreateCylinder(world, 0.25f, 0.25f, 0.5f, 0, NULL);
	NewtonCollisionSetMode(collision, 0);
	NewtonBody* const differentialBody = NewtonCreateDynamicBody(world, collision, &matrix[0][0]);

	// use the tire mass to set the mass of the differential	
	NewtonBodyGetMass(leftTire->GetBody0(), &mass, &Ixx, &Iyy, &Izz);
	NewtonBodySetMassMatrix(differentialBody, mass, Ixx, Ixx, Ixx);

	dVector drag(0.0f);
	NewtonBodySetLinearDamping(differentialBody, 0);
	NewtonBodySetAngularDamping(differentialBody, &drag[0]);
	NewtonBodySetMaxRotationPerStep(differentialBody, 3.141692f * 2.0f);
	NewtonBodySetForceAndTorqueCallback(differentialBody, m_forceAndTorqueCallback);
	NewtonDestroyCollision(collision);

	dMatrix pinMatrix(matrix);
	pinMatrix.m_front = matrix.m_front;
	pinMatrix.m_up = matrix.m_right;
	pinMatrix.m_right = pinMatrix.m_front.CrossProduct(pinMatrix.m_up);
	dDifferentialJoint* const differentialJoint = new dDifferentialJoint(pinMatrix, differentialBody, m_body);
	m_differentialList.Append(differentialJoint);

//	NewtonBody* const chassisBody = GetBody();
//	NewtonBody* const differentialBody = differential->GetBody();
//	m_bodyPartsList.Append(differential);

	m_bodyList.Append(differentialBody);
	NewtonCollisionAggregateAddBody(m_collisionAggregate, differentialBody);

	dMatrix chassisMatrix;
	dMatrix differentialMatrix;
	NewtonBodyGetMatrix(m_body, &chassisMatrix[0][0]);
	NewtonBodyGetMatrix(differentialBody, &differentialMatrix[0][0]);
	
	dMatrix leftTireMatrix;
	NewtonBody* const leftTireBody = leftTire->GetBody0();
	NewtonBodyGetMatrix(leftTireBody, &leftTireMatrix[0][0]);
	leftTireMatrix = leftTire->GetMatrix0() * leftTireMatrix;
	new dAxelJoint(leftTireMatrix[0], differentialMatrix[0].Scale(-1.0f), chassisMatrix[2], leftTireBody, differentialBody, m_body);

	dMatrix rightTireMatrix;
	NewtonBody* const rightTireBody = rightTire->GetBody0();
	NewtonBodyGetMatrix(rightTireBody, &rightTireMatrix[0][0]);
	rightTireMatrix = rightTire->GetMatrix0() * rightTireMatrix;
	new dAxelJoint(rightTireMatrix[0], differentialMatrix[0].Scale(1.0f), chassisMatrix[2], rightTireBody, differentialBody, m_body);

	return differentialJoint;
}

dDifferentialJoint* dCustomVehicleController::AddDifferential(dDifferentialJoint* const leftDifferential, dDifferentialJoint* const rightDifferential)
{
	dMatrix matrix;
	dVector origin;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;

//	dList<dBodyPartDifferential>::dListNode* const differentialNode = m_differentialList.Append();
//	dBodyPartDifferential* const differential = &differentialNode->GetInfo();
//	differential->Init(this, mass, Ixx);

	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = ((dCustomVehicleControllerManager*)manager)->GetWorld();

	// get engine location (place at the body center of mass)
	NewtonBodyGetCentreOfMass(m_body, &origin[0]);
	NewtonBodyGetMatrix(m_body, &matrix[0][0]);
	matrix.m_posit = matrix.TransformVector(origin);

	NewtonCollision* const collision = NewtonCreateSphere(world, 0.1f, 0, NULL);
	//NewtonCollision* const collision = NewtonCreateCylinder(world, 0.25f, 0.25f, 0.5f, 0, NULL);
	NewtonCollisionSetMode(collision, 0);
	NewtonBody* const differentialBody = NewtonCreateDynamicBody(world, collision, &matrix[0][0]);

	// use the tire mass to set the mass of the differential	
	NewtonBodyGetMass(leftDifferential->GetBody0(), &mass, &Ixx, &Iyy, &Izz);
	NewtonBodySetMassMatrix(differentialBody, mass, Ixx, Ixx, Ixx);

	dVector drag(0.0f);
	NewtonBodySetLinearDamping(differentialBody, 0);
	NewtonBodySetAngularDamping(differentialBody, &drag[0]);
	NewtonBodySetMaxRotationPerStep(differentialBody, 3.141692f * 2.0f);
	NewtonBodySetForceAndTorqueCallback(differentialBody, m_forceAndTorqueCallback);
	NewtonDestroyCollision(collision);

	dMatrix pinMatrix(matrix);
	pinMatrix.m_front = matrix.m_front;
	pinMatrix.m_up = matrix.m_right;
	pinMatrix.m_right = pinMatrix.m_front.CrossProduct(pinMatrix.m_up);
	dDifferentialJoint* const differentialJoint = new dDifferentialJoint(pinMatrix, differentialBody, m_body);
	m_differentialList.Append(differentialJoint);

//	NewtonBody* const chassisBody = GetBody();
//	NewtonBody* const differentialBody = differential->GetBody();
//	m_bodyPartsList.Append(differential);
//	NewtonCollisionAggregateAddBody(m_collisionAggregate, differentialBody);

	m_bodyList.Append(differentialBody);
	NewtonCollisionAggregateAddBody(m_collisionAggregate, differentialBody);

	dMatrix chassisMatrix;
	dMatrix differentialMatrix;
	NewtonBodyGetMatrix(m_body, &chassisMatrix[0][0]);
	NewtonBodyGetMatrix(differentialBody, &differentialMatrix[0][0]);

	dMatrix leftDifferentialMatrix;
	NewtonBody* const leftDifferentialBody = leftDifferential->GetBody0();
	NewtonBodyGetMatrix(leftDifferentialBody, &leftDifferentialMatrix[0][0]);
	leftDifferentialMatrix = leftDifferential->GetMatrix0() * leftDifferentialMatrix;
	new dAxelJoint(leftDifferentialMatrix[1], differentialMatrix[0].Scale(-1.0f), chassisMatrix[2], leftDifferentialBody, differentialBody, m_body);

	dMatrix rightDifferentialMatrix;
	NewtonBody* const rightDifferentialBody = rightDifferential->GetBody0();
	NewtonBodyGetMatrix(rightDifferentialBody, &rightDifferentialMatrix[0][0]);
	rightDifferentialMatrix = rightDifferential->GetMatrix0() * rightDifferentialMatrix;
	new dAxelJoint(rightDifferentialMatrix[1], differentialMatrix[0].Scale(1.0f), chassisMatrix[2], rightDifferentialBody, differentialBody, m_body);

	return differentialJoint;
}


void dCustomVehicleController::LinkTiresKinematically(dWheelJoint* const tire0, dWheelJoint* const tire1)
{
	dAssert (0);
/*
	dWheelJoint* const joint0 = tire0;
	dWheelJoint* const joint1 = tire1;

	dMatrix matrix;
	dMatrix tireMatrix0;
	dMatrix tireMatrix1;
	joint0->CalculateGlobalMatrix(tireMatrix0, matrix);
	joint1->CalculateGlobalMatrix(tireMatrix1, matrix);

	dFloat gearRatio = tire0->m_data.m_radio / tire1->m_data.m_radio;
	new dCustomGear(gearRatio, tireMatrix0.m_front, tireMatrix1.m_front.Scale (-1.0f), tire0->GetBody(), tire1->GetBody());
*/
}

dEngineJoint* dCustomVehicleController::GetEngineJoint() const
{
	return m_engine;
}

dEngineJoint* dCustomVehicleController::AddEngineJoint (dFloat mass, dFloat armatureRadius)
{
	dMatrix matrix;
	dVector origin;

//	m_engine = new dBodyPartEngine (this, mass, armatureRadius);
//	m_parent = &controller->m_chassis;
//	m_controller = controller;
//	NewtonBody* const chassisBody = m_controller->GetBody();
//	NewtonWorld* const world = ((dCustomVehicleControllerManager*)m_controller->GetManager())->GetWorld();

	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = ((dCustomVehicleControllerManager*)manager)->GetWorld();

	// get engine location (place at the body center of mass)
	NewtonBodyGetCentreOfMass(m_body, &origin[0]);
	NewtonBodyGetMatrix(m_body, &matrix[0][0]);

	//origin.m_y += 2.0f;
	matrix.m_posit = matrix.TransformVector(origin);

	NewtonCollision* const collision = NewtonCreateSphere(world, 0.1f, 0, NULL);
	//NewtonCollision* const collision = NewtonCreateCylinder(world, 0.1f, 0.1f, 0.5f, 0, NULL);
	NewtonCollisionSetMode(collision, 0);
	NewtonBody* const engineBody = NewtonCreateDynamicBody(world, collision, &matrix[0][0]);

	// make engine inertia spherical (also make scale inertia but twice the radio for more stability)
	const dFloat engineInertiaScale = 9.0f;
	const dFloat inertia = 2.0f * engineInertiaScale * mass * armatureRadius * armatureRadius / 5.0f;
	NewtonBodySetMassMatrix(engineBody, mass, inertia, inertia, inertia);

	dVector drag(0.0f);
	NewtonBodySetLinearDamping(engineBody, 0);
	NewtonBodySetAngularDamping(engineBody, &drag[0]);
	NewtonBodySetMaxRotationPerStep(engineBody, 3.141692f * 2.0f);
	NewtonBodySetForceAndTorqueCallback(engineBody, m_forceAndTorqueCallback);
	NewtonDestroyCollision(collision);

	dMatrix pinMatrix(matrix);
	pinMatrix.m_front = matrix.m_right;
	pinMatrix.m_up = matrix.m_up;
	pinMatrix.m_right = pinMatrix.m_front.CrossProduct(pinMatrix.m_up);
	m_engine = new dEngineJoint(pinMatrix, engineBody, m_body);

//	m_bodyPartsList.Append(m_engine);
//	NewtonCollisionAggregateAddBody(m_collisionAggregate, m_engine->GetBody());
	m_bodyList.Append(engineBody);
	NewtonCollisionAggregateAddBody(m_collisionAggregate, engineBody);
	return m_engine;
}

dVector dCustomVehicleController::GetTireNormalForce(const dWheelJoint* const tire) const
{
//	dWheelJoint* const joint = tire->GetJoint();
//	dFloat force = joint->GetTireLoad();
	dFloat force = tire->GetTireLoad();
	return dVector (0.0f, force, 0.0f, 0.0f);
}

dVector dCustomVehicleController::GetTireLateralForce(const dWheelJoint* const tire) const
{
	return tire->GetLateralForce();
}

dVector dCustomVehicleController::GetTireLongitudinalForce(const dWheelJoint* const tire) const
{
	return tire->GetLongitudinalForce();
}

dFloat dCustomVehicleController::GetAerodynamicsDowforceCoeficient() const
{
	return m_aerodynamicsDownForceCoefficient;
}

void dCustomVehicleController::SetAerodynamicsDownforceCoefficient(dFloat downWeightRatioAtSpeedFactor, dFloat speedFactor, dFloat maxWeightAtTopSpeed)
{
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;

	dAssert (speedFactor >= 0.0f);
	dAssert (speedFactor <= 1.0f);
	dAssert (downWeightRatioAtSpeedFactor >= 0.0f);
	dAssert (downWeightRatioAtSpeedFactor < maxWeightAtTopSpeed);
	NewtonBody* const body = GetBody();
	NewtonBodyGetMass(body, &mass, &Ixx, &Iyy, &Izz);
	dFloat topSpeed = m_engineControl ? m_engineControl->GetTopSpeed() : 25.0f;
	m_aerodynamicsDownSpeedCutOff = topSpeed * speedFactor;
	m_aerodynamicsDownForce0 = mass * downWeightRatioAtSpeedFactor * dAbs(m_gravityMag);
	m_aerodynamicsDownForce1 = mass * maxWeightAtTopSpeed * dAbs(m_gravityMag);
	m_aerodynamicsDownForceCoefficient = m_aerodynamicsDownForce0 / (m_aerodynamicsDownSpeedCutOff * m_aerodynamicsDownSpeedCutOff);
}

int dCustomVehicleControllerManager::OnTireAABBOverlap(const NewtonMaterial* const material, const NewtonBody* const body0, const NewtonBody* const body1, int threadIndex)
{
	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)NewtonMaterialGetMaterialPairUserData(material);

	const NewtonCollision* const collision0 = NewtonBodyGetCollision(body0);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		const dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(collision0);
		dAssert(tire->GetBody1() != otherBody);
		return manager->OnTireAABBOverlap(material, tire, otherBody);
	} 
	const NewtonCollision* const collision1 = NewtonBodyGetCollision(body1);
	dAssert (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData) ;
	const NewtonBody* const otherBody = body0;
	const dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(collision1);
	dAssert(tire->GetBody1() != otherBody);
	return manager->OnTireAABBOverlap(material, tire, otherBody);
}

int dCustomVehicleControllerManager::OnTireAABBOverlap(const NewtonMaterial* const material, const dWheelJoint* const tire, const NewtonBody* const otherBody) const
{
	for (int i = 0; i < tire->m_collidingCount; i ++) {
		if (otherBody == tire->m_contactInfo[i].m_hitBody) {
			return 1;
		}
	}
	return tire->m_hasFender ? 0 : 1;
}

int dCustomVehicleControllerManager::OnContactGeneration (const NewtonMaterial* const material, const NewtonBody* const body0, const NewtonCollision* const collision0, const NewtonBody* const body1, const NewtonCollision* const collision1, NewtonUserContactPoint* const contactBuffer, int maxCount, int threadIndex)
{
	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*) NewtonMaterialGetMaterialPairUserData(material);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
//	const void* const data1 = NewtonCollisionDataPointer(collision1);
	dAssert ((data0 == manager->m_tireShapeTemplateData) || (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData));
	dAssert (!((data0 == manager->m_tireShapeTemplateData) && (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData)));

	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		const NewtonCollision* const tireCollision = collision0;
		const NewtonCollision* const otherCollision = collision1;
		const dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(tireCollision);
		dAssert (tire->GetBody0() == body0);
		return manager->OnContactGeneration (tire, otherBody, otherCollision, contactBuffer, maxCount, threadIndex);
	} 
	dAssert (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData);
	const NewtonBody* const otherBody = body0;
	const NewtonCollision* const tireCollision = collision1;
	const NewtonCollision* const otherCollision = collision0;
	const dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(tireCollision);
	dAssert (tire->GetBody1() == body1);
	int count = manager->OnContactGeneration(tire, otherBody, otherCollision, contactBuffer, maxCount, threadIndex);

	for (int i = 0; i < count; i ++) {	
		contactBuffer[i].m_normal[0] *= -1.0f;
		contactBuffer[i].m_normal[1] *= -1.0f;
		contactBuffer[i].m_normal[2] *= -1.0f;
		dSwap (contactBuffer[i].m_shapeId0, contactBuffer[i].m_shapeId1);
	}
	return count;
}


int dCustomVehicleControllerManager::Collide(dWheelJoint* const tire, int threadIndex) const
{
	dMatrix tireMatrix;
	dMatrix chassisMatrix;

	const NewtonWorld* const world = GetWorld();
	const NewtonBody* const tireBody = tire->GetBody0();
	const NewtonBody* const vehicleBody = tire->GetBody1();
	dAssert (tireBody == tire->GetTireBody());
	dCustomVehicleController* const controller = tire->GetController();

	NewtonBodyGetMatrix(tireBody, &tireMatrix[0][0]);
	NewtonBodyGetMatrix(vehicleBody, &chassisMatrix[0][0]);
//	tire->CalculateGlobalMatrix(tireMatrix, chassisMatrix);
//	const dVector& tireSidePin = tireMatrix.m_front;
	const dVector tireSidePin (tireMatrix.RotateVector(tire->GetMatrix0().m_front));
	chassisMatrix = tire->GetMatrix1() * chassisMatrix;
	chassisMatrix.m_posit += tireSidePin.Scale(tireSidePin.DotProduct3(tireMatrix.m_posit - chassisMatrix.m_posit));

	dVector suspensionSpan (chassisMatrix.m_up.Scale(tire->m_suspensionLength));

	dMatrix tireSweeptMatrix;
	tireSweeptMatrix.m_up = chassisMatrix.m_up;
	tireSweeptMatrix.m_right = tireSidePin.CrossProduct(chassisMatrix.m_up);
	tireSweeptMatrix.m_right = tireSweeptMatrix.m_right.Scale(1.0f / dSqrt(tireSweeptMatrix.m_right.DotProduct3(tireSweeptMatrix.m_right)));
	tireSweeptMatrix.m_front = tireSweeptMatrix.m_up.CrossProduct(tireSweeptMatrix.m_right);
	tireSweeptMatrix.m_posit = chassisMatrix.m_posit + suspensionSpan;

	NewtonCollision* const tireCollision = NewtonBodyGetCollision(tireBody);
	dTireFilter filter(tire, controller);

	dFloat timeOfImpact;
	tire->m_collidingCount = 0;
	const int maxContactCount = 2;
	dAssert (sizeof (tire->m_contactInfo) / sizeof (tire->m_contactInfo[0]) > 2);
	int count = NewtonWorldConvexCast (world, &tireSweeptMatrix[0][0], &chassisMatrix.m_posit[0], tireCollision, &timeOfImpact, &filter, dCustomControllerConvexCastPreFilter::Prefilter, tire->m_contactInfo, maxContactCount, threadIndex);

	if (timeOfImpact < 1.0e-2f) {
		class CheckBadContact: public dTireFilter
		{
			public:
			CheckBadContact(const dWheelJoint* const tire, const dCustomVehicleController* const controller, int oldCount, NewtonWorldConvexCastReturnInfo* const oldInfo)
				:dTireFilter(tire, controller)
				,m_oldCount(oldCount) 
				,m_oldInfo(oldInfo)
			{
			}

			unsigned Prefilter(const NewtonBody* const body, const NewtonCollision* const myCollision)
			{
				for (int i = 0; i < m_oldCount; i ++) {
					if (body == m_oldInfo[i].m_hitBody) {
						return 0;
					}
				}

				return dTireFilter::Prefilter(body, myCollision);
			}

			int m_oldCount;
			NewtonWorldConvexCastReturnInfo* m_oldInfo;
		};
			
		
		dFloat timeOfImpact1;
		NewtonWorldConvexCastReturnInfo contactInfo[4];
		CheckBadContact checkfilter (tire, controller, count, tire->m_contactInfo);
		int count1 = NewtonWorldConvexCast (world, &tireSweeptMatrix[0][0], &chassisMatrix.m_posit[0], tireCollision, &timeOfImpact1, &checkfilter, dCustomControllerConvexCastPreFilter::Prefilter, contactInfo, maxContactCount, threadIndex);
		if (count1) {
			count = count1;
			timeOfImpact = timeOfImpact1;
			for (int i = 0; i < count; i ++) {
				tire->m_contactInfo[i] = contactInfo[i];
			}
		}
	}


	if (count) {
		timeOfImpact = 1.0f - timeOfImpact;
		dFloat num = (tireMatrix.m_posit - chassisMatrix.m_up.Scale (0.25f * tire->m_suspensionLength) - chassisMatrix.m_posit).DotProduct3(suspensionSpan);
		dFloat tireParam = dMax (num / (tire->m_suspensionLength * tire->m_suspensionLength), dFloat(0.0f));

		if (tireParam <= timeOfImpact) {
			tireSweeptMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(timeOfImpact * tire->m_suspensionLength);
			for (int i = count - 1; i >= 0; i --) {
				dVector p (tireSweeptMatrix.UntransformVector (dVector (tire->m_contactInfo[i].m_point[0], tire->m_contactInfo[i].m_point[1], tire->m_contactInfo[i].m_point[2], 1.0f)));
				if ((p.m_y >= -(tire->m_radio * 0.5f)) || (dAbs (p.m_x / p.m_y) > 0.4f)) {
					tire->m_contactInfo[i] = tire->m_contactInfo[count - 1];
					count --;
				}
			}
			if (count) {

//				dFloat x1 = timeOfImpact * tire->m_data.m_suspesionlenght;
//				dFloat x0 = (tireMatrix.m_posit - chassisMatrix.m_posit).DotProduct3(chassisMatrix.m_up);
//				dFloat x10 = x1 - x0;
//				if (x10 > (1.0f / 32.0f)) {
//					dFloat param = 1.0e10f;
//					x1 = x0 + (1.0f / 32.0f);
//					dMatrix origin (chassisMatrix);
//					origin.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(x1);
//					NewtonWorldConvexCast (world, &chassisMatrix[0][0], &tireSweeptMatrix.m_posit[0], tireCollision, &param, &filter, dCustomControllerConvexCastPreFilter::Prefilter, NULL, 0, threadIndex);
//					count = (param < 1.0f) ? 0 : count;
//				}
//				if (count) {
//					tireMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(x1);
//					NewtonBodySetMatrixNoSleep(tireBody, &tireMatrix[0][0]);
//				}

				dFloat x = timeOfImpact * tire->m_suspensionLength;
				dFloat step = (tireSweeptMatrix.m_posit - tireMatrix.m_posit).DotProduct3(chassisMatrix.m_up);
				if (step < -1.0f / 32.0f) {
					count = 0;
				}

				if (count) {
					tireMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(x);
					NewtonBodySetMatrixNoSleep(tireBody, &tireMatrix[0][0]);
				}
			}
		} else {
			count = 0;
		}
	}

	tire->m_collidingCount = count;
	if (!tire->m_hasFender) {
		count = NewtonWorldCollide (world, &tireMatrix[0][0], tireCollision, &filter, dCustomControllerConvexCastPreFilter::Prefilter, &tire->m_contactInfo[count], maxContactCount, threadIndex);
		for (int i = 0; i < count; i++) {
			if (tire->m_contactInfo[tire->m_collidingCount + i].m_penetration == 0.0f) {
				tire->m_contactInfo[tire->m_collidingCount + i].m_penetration = 1.0e-5f;
			}
		}
		tire->m_collidingCount += count;
	}

	return tire->m_collidingCount ? 1 : 0;
}


void dCustomVehicleControllerManager::OnTireContactsProcess (const NewtonJoint* const contactJoint, dFloat timestep, int threadIndex)
{
	void* const contact = NewtonContactJointGetFirstContact(contactJoint);
	dAssert (contact);
	NewtonMaterial* const material = NewtonContactGetMaterial(contact);
	dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*) NewtonMaterialGetMaterialPairUserData(material);

	const NewtonBody* const body0 = NewtonJointGetBody0(contactJoint);
	const NewtonBody* const body1 = NewtonJointGetBody1(contactJoint);
	const NewtonCollision* const collision0 = NewtonBodyGetCollision(body0);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(collision0);
		dAssert(tire->GetBody1() != otherBody);
		manager->OnTireContactsProcess(contactJoint, tire, otherBody, timestep);
	} else {
		const NewtonCollision* const collision1 = NewtonBodyGetCollision(body1);
		const void* const data1 = NewtonCollisionDataPointer(collision1);
		if (data1 == manager->m_tireShapeTemplateData) {
			const NewtonCollision* const collision2 = NewtonBodyGetCollision(body1);
			dAssert(NewtonCollisionDataPointer(collision2) == manager->m_tireShapeTemplateData);
			const NewtonBody* const otherBody = body0;
			dWheelJoint* const tire = (dWheelJoint*) NewtonCollisionGetUserData1(collision2);
			dAssert(tire->GetBody1() != otherBody);
			manager->OnTireContactsProcess(contactJoint, tire, otherBody, timestep);
		}
	}
}


int dCustomVehicleControllerManager::OnContactGeneration(const dWheelJoint* const tire, const NewtonBody* const otherBody, const NewtonCollision* const othercollision, NewtonUserContactPoint* const contactBuffer, int maxCount, int threadIndex) const
{
	int count = 0;
	NewtonCollision* const collisionA = NewtonBodyGetCollision(tire->GetBody0());
	dAssert (tire->GetBody0() == tire->GetTireBody());
	dLong tireID = NewtonCollisionGetUserID(collisionA);
	for (int i = 0; i < tire->m_collidingCount; i++) {
		if (otherBody == tire->m_contactInfo[i].m_hitBody) {
			contactBuffer[count].m_point[0] = tire->m_contactInfo[i].m_point[0];
			contactBuffer[count].m_point[1] = tire->m_contactInfo[i].m_point[1];
			contactBuffer[count].m_point[2] = tire->m_contactInfo[i].m_point[2];
			contactBuffer[count].m_point[3] = 1.0f;
			contactBuffer[count].m_normal[0] = tire->m_contactInfo[i].m_normal[0];
			contactBuffer[count].m_normal[1] = tire->m_contactInfo[i].m_normal[1];
			contactBuffer[count].m_normal[2] = tire->m_contactInfo[i].m_normal[2];
			contactBuffer[count].m_normal[3] = 0.0f;
			contactBuffer[count].m_penetration = tire->m_contactInfo[i].m_penetration;
			contactBuffer[count].m_shapeId0 = tireID;
			contactBuffer[count].m_shapeId1 = tire->m_contactInfo[i].m_contactID;
			count++;
		}
	}
	return count;
}

void dCustomVehicleControllerManager::OnTireContactsProcess(const NewtonJoint* const contactJoint, dWheelJoint* const tire, const NewtonBody* const otherBody, dFloat timestep)
{
	dMatrix tireMatrix;
	dMatrix chassisMatrix;
	dVector tireOmega(0.0f);
	dVector tireVeloc(0.0f);

	NewtonBody* const tireBody = tire->GetBody0();
	dAssert (tireBody == tire->GetTireBody());
	dAssert((tireBody == NewtonJointGetBody0(contactJoint)) || (tireBody == NewtonJointGetBody1(contactJoint)));
	const dCustomVehicleController* const controller = tire->GetController();
	
	tire->CalculateGlobalMatrix(tireMatrix, chassisMatrix);

	NewtonBodyGetOmega(tireBody, &tireOmega[0]);
	NewtonBodyGetVelocity(tireBody, &tireVeloc[0]);

	dVector lateralPin (tireMatrix.m_front);
	dVector longitudinalPin (tireMatrix.m_front.CrossProduct(chassisMatrix.m_up));

	tire->m_lateralSlip = 0.0f;
	tire->m_aligningTorque = 0.0f;
	tire->m_longitudinalSlip = 0.0f;

	for (void* contact = NewtonContactJointGetFirstContact(contactJoint); contact; contact = NewtonContactJointGetNextContact(contactJoint, contact)) {
		dVector contactPoint(0.0f);
		dVector contactNormal(0.0f);

		NewtonMaterial* const material = NewtonContactGetMaterial(contact);
		NewtonMaterialContactRotateTangentDirections(material, &lateralPin[0]);
		NewtonMaterialGetContactPositionAndNormal(material, tireBody, &contactPoint[0], &contactNormal[0]);
			
		dVector tireAnglePin(contactNormal.CrossProduct(lateralPin));
		dFloat pinMag2 = tireAnglePin.DotProduct3(tireAnglePin);
		if (pinMag2 > 0.25f) {
			// brush rubber tire friction model
			// project the contact point to the surface of the collision shape
			dVector contactPatch(contactPoint - lateralPin.Scale((contactPoint - tireMatrix.m_posit).DotProduct3(lateralPin)));
			dVector dp(contactPatch - tireMatrix.m_posit);
			dVector radius(dp.Scale(tire->m_radio / dSqrt(dp.DotProduct3(dp))));

			dVector lateralContactDir(0.0f);
			dVector longitudinalContactDir(0.0f);
			NewtonMaterialGetContactTangentDirections(material, tireBody, &lateralContactDir[0], &longitudinalContactDir[0]);

			dFloat tireOriginLongitudinalSpeed = tireVeloc.DotProduct3(longitudinalContactDir);
			dFloat tireContactLongitudinalSpeed = -longitudinalContactDir.DotProduct3 (tireOmega.CrossProduct(radius));

			if ((dAbs(tireOriginLongitudinalSpeed) < (1.0f)) || (dAbs(tireContactLongitudinalSpeed) < 0.1f)) {
				// vehicle moving low speed, do normal coulomb friction
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
			} else {

				// calculating Brush tire model with longitudinal and lateral coupling 
				// for friction coupling according to Motor Vehicle dynamics by: Giancarlo Genta 
				// reduces to this, which may have a divide by zero locked, so I am clamping to some small value
				// dFloat k = (vw - vx) / vx;
				if (dAbs(tireContactLongitudinalSpeed) < 0.01f) {
					tireContactLongitudinalSpeed = 0.01f * dSign(tireContactLongitudinalSpeed);
				}
				tire->m_longitudinalSlip = (tireContactLongitudinalSpeed - tireOriginLongitudinalSpeed) / tireOriginLongitudinalSpeed;

				dFloat lateralSpeed = tireVeloc.DotProduct3(lateralPin);
				dFloat longitudinalSpeed = tireVeloc.DotProduct3(longitudinalPin);
				dAssert (dAbs (longitudinalSpeed) > 0.01f);

				tire->m_lateralSlip = dAbs(lateralSpeed / longitudinalSpeed);
	
				dFloat aligningMoment;
				dFloat lateralFrictionCoef;
				dFloat longitudinalFrictionCoef;
				dFloat lateralSlipSensitivity = 2.0f;
				controller->m_contactFilter->CalculateTireFrictionCoefficents(tire, otherBody, material, 
					tire->m_longitudinalSlip, tire->m_lateralSlip * lateralSlipSensitivity, 
					tire->m_longitudialStiffness, tire->m_lateralStiffness, 
					longitudinalFrictionCoef, lateralFrictionCoef, aligningMoment);

//dTrace (("%d %f %f\n", tire->m_index, longitudinalFrictionCoef, lateralFrictionCoef));

				NewtonMaterialSetContactFrictionCoef(material, lateralFrictionCoef, lateralFrictionCoef, 0);
				NewtonMaterialSetContactFrictionCoef(material, longitudinalFrictionCoef, longitudinalFrictionCoef, 1);
#if 0
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
#endif
			}

		} else {
			// vehicle moving low speed, do normal coulomb friction
			NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
			NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
		}

		NewtonMaterialSetContactElasticity(material, 0.0f);
	}
}


dVector dCustomVehicleController::GetLastLateralForce(dWheelJoint* const tire) const
{
	return (GetTireLateralForce(tire) + GetTireLongitudinalForce(tire)).Scale (-1.0f);
}


void dCustomVehicleController::ApplySuspensionForces(dFloat timestep) const
{
	dMatrix chassisMatrix;
	dMatrix chassisInvInertia;
	dVector chassisOrigin(0.0f);
	dVector chassisForce(0.0f);
	dVector chassisTorque(0.0f);

	const int maxSize = 64;
	dComplentaritySolver::dJacobianPair m_jt[maxSize];
	dComplentaritySolver::dJacobianPair m_jInvMass[maxSize];
	dWheelJoint* tires[maxSize];
	dFloat accel[maxSize];
	dFloat massMatrix[maxSize * maxSize];
	dFloat chassisMass;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;

	NewtonBody* const chassisBody = GetBody();
	NewtonBodyGetCentreOfMass(chassisBody, &chassisOrigin[0]);
	NewtonBodyGetMatrix(chassisBody, &chassisMatrix[0][0]);
	NewtonBodyGetInvMass(chassisBody, &chassisMass, &Ixx, &Iyy, &Izz);
	NewtonBodyGetInvInertiaMatrix(chassisBody, &chassisInvInertia[0][0]);

	chassisOrigin = chassisMatrix.TransformVector(chassisOrigin);
	int tireCount = 0;
	for (dList<dWheelJoint*>::dListNode* tireNode = m_tireList.GetFirst(); tireNode; tireNode = tireNode->GetNext()) {
		dWheelJoint* const tire = tireNode->GetInfo();

		if (tire->m_suspentionType != dSuspensionType::m_roller) {
			tires[tireCount] = tire;

			NewtonBody* const tireBody = tire->GetTireBody();
			dAssert(tireBody == tire->GetBody0());
			dAssert(chassisBody == tire->GetBody1());

			dMatrix tireMatrix;
			//dMatrix chassisMatrix;
			dVector tireVeloc(0.0f);
			dVector chassisPivotVeloc(0.0f);

			tire->CalculateGlobalMatrix(tireMatrix, chassisMatrix);
			NewtonBodyGetVelocity(tireBody, &tireVeloc[0]);
			NewtonBodyGetPointVelocity(chassisBody, &tireMatrix.m_posit[0], &chassisPivotVeloc[0]);

			dFloat param = tire->CalculateTireParametricPosition(tireMatrix, chassisMatrix);
			param = dClamp(param, dFloat(-0.25f), dFloat(1.0f));

			dFloat x = tire->m_suspensionLength * param;
			dFloat v = ((tireVeloc - chassisPivotVeloc).DotProduct3(chassisMatrix.m_up));

			dFloat weight = 1.0f;
			switch (tire->m_suspentionType)
			{
				case dSuspensionType::m_offroad:
					weight = 0.9f;
					break;
				case dSuspensionType::m_confort:
					weight = 1.0f;
					break;
				case dSuspensionType::m_race:
					weight = 1.1f;
					break;
			}
			accel[tireCount] = -NewtonCalculateSpringDamperAcceleration(timestep, tire->m_springStrength * weight, x, tire->m_dampingRatio, v);

			dMatrix tireInvInertia;
			dFloat tireMass;

			NewtonBodyGetInvMass(tireBody, &tireMass, &Ixx, &Iyy, &Izz);
			NewtonBodyGetInvInertiaMatrix(tireBody, &tireInvInertia[0][0]);

			m_jt[tireCount].m_jacobian_IM0.m_linear = chassisMatrix.m_up.Scale(-1.0f);
			m_jt[tireCount].m_jacobian_IM0.m_angular = dVector(0.0f);
			m_jt[tireCount].m_jacobian_IM1.m_linear = chassisMatrix.m_up;
			m_jt[tireCount].m_jacobian_IM1.m_angular = (tireMatrix.m_posit - chassisOrigin).CrossProduct(chassisMatrix.m_up);

			m_jInvMass[tireCount].m_jacobian_IM0.m_linear = m_jt[tireCount].m_jacobian_IM0.m_linear.Scale(tireMass);
			m_jInvMass[tireCount].m_jacobian_IM0.m_angular = tireInvInertia.RotateVector(m_jt[tireCount].m_jacobian_IM0.m_angular);
			m_jInvMass[tireCount].m_jacobian_IM1.m_linear = m_jt[tireCount].m_jacobian_IM1.m_linear.Scale(chassisMass);
			m_jInvMass[tireCount].m_jacobian_IM1.m_angular = chassisInvInertia.RotateVector(m_jt[tireCount].m_jacobian_IM1.m_angular);

			tireCount++;
		}
	}

	for (int i = 0; i < tireCount; i++) {
		dFloat* const row = &massMatrix[i * tireCount];
		dFloat aii = m_jInvMass[i].m_jacobian_IM0.m_linear.DotProduct3(m_jt[i].m_jacobian_IM0.m_linear) + m_jInvMass[i].m_jacobian_IM0.m_angular.DotProduct3(m_jt[i].m_jacobian_IM0.m_angular) +
					 m_jInvMass[i].m_jacobian_IM1.m_linear.DotProduct3(m_jt[i].m_jacobian_IM1.m_linear) + m_jInvMass[i].m_jacobian_IM1.m_angular.DotProduct3(m_jt[i].m_jacobian_IM1.m_angular);

		row[i] = aii * 1.0001f;
		for (int j = i + 1; j < tireCount; j++) {
			dFloat aij = m_jInvMass[i].m_jacobian_IM1.m_linear.DotProduct3(m_jt[j].m_jacobian_IM1.m_linear) + m_jInvMass[i].m_jacobian_IM1.m_angular.DotProduct3(m_jt[j].m_jacobian_IM1.m_angular);
			row[j] = aij;
			massMatrix[j * tireCount + i] = aij;
		}
	}

	dCholeskyFactorization(tireCount, massMatrix);
	dCholeskySolve(tireCount, tireCount, massMatrix, accel);
	for (int i = 0; i < tireCount; i++) {
		NewtonBody* const tirebody = tires[i]->GetTireBody();
		dVector tireForce(m_jt[i].m_jacobian_IM0.m_linear.Scale(accel[i]));
		dVector tireTorque(m_jt[i].m_jacobian_IM0.m_angular.Scale(accel[i]));
		NewtonBodyAddForce(tirebody, &tireForce[0]);
		NewtonBodyAddTorque(tirebody, &tireTorque[0]);
		chassisForce += m_jt[i].m_jacobian_IM1.m_linear.Scale(accel[i]);
		chassisTorque += m_jt[i].m_jacobian_IM1.m_angular.Scale(accel[i]);
	}
	NewtonBodyAddForce(chassisBody, &chassisForce[0]);
	NewtonBodyAddTorque(chassisBody, &chassisTorque[0]);
}


void dCustomVehicleController::CalculateSideSlipDynamics(dFloat timestep)
{
	dMatrix matrix;
	dVector veloc;

	NewtonBody* const chassisBody = GetBody();
	NewtonBodyGetMatrix(chassisBody, &matrix[0][0]);
	NewtonBodyGetVelocity(chassisBody, &veloc[0]);

	dFloat speed_x = veloc.DotProduct3(matrix.m_front);
	dFloat speed_z = veloc.DotProduct3(matrix.m_right);
	if (dAbs(speed_x) > 1.0f) {
		m_prevSideSlip = m_sideSlip;
		m_sideSlip = speed_z / dAbs(speed_x);
	} else {
		m_sideSlip = 0.0f;
		m_prevSideSlip = 0.0f;
	}

static int xxx;
//dTrace (("\n%d b(%f) rate(%f)\n", xxx, m_sideSlip * 180.0f/3.1416f, (m_sideSlip - m_prevSideSlip) * (180.0f / 3.1416f) / timestep));
xxx ++;

if ((dAbs (m_sideSlip * 180.0f/3.1416f) > 35.0f))  {
	dVector xxx1 (matrix.m_up.Scale (-8000.0f * dSign(m_sideSlip)));
	NewtonBodyAddTorque (chassisBody, &xxx1[0]);
} else {
	dFloat betaRate = (m_sideSlip - m_prevSideSlip) / timestep;
	if (dAbs (betaRate * 180.0f/3.1416f) > 15.0f) {
		dVector xxx1 (matrix.m_up.Scale (-8000.0f * dSign(betaRate)));
		NewtonBodyAddTorque (chassisBody, &xxx1[0]);
	}
}

}


void dCustomVehicleController::ApplyDownForce()
{
	// add aerodynamics forces
	dMatrix matrix;
	dVector veloc(0.0f);

	NewtonBody* const body = GetBody();
	NewtonBodyGetVelocity(body, &veloc[0]);
	NewtonBodyGetMatrix(body, &matrix[0][0]);

	veloc -= matrix.m_up.Scale(veloc.DotProduct3(matrix.m_up));
	dFloat downForceMag = m_aerodynamicsDownForceCoefficient * veloc.DotProduct3(veloc);
	if (downForceMag > m_aerodynamicsDownForce0) {
		dFloat speed = dSqrt(veloc.DotProduct3(veloc));
		dFloat topSpeed = GetEngine() ? GetEngine()->GetTopGear() : 30.0f;
		dFloat speedRatio = (speed - m_aerodynamicsDownSpeedCutOff) / (topSpeed - speed);
		downForceMag = m_aerodynamicsDownForce0 + (m_aerodynamicsDownForce1 - m_aerodynamicsDownForce0) * speedRatio;
	}

	dVector downforce(matrix.m_up.Scale(-downForceMag));
	NewtonBodyAddForce(body, &downforce[0]);
}


void dCustomVehicleController::PostUpdate(dFloat timestep, int threadIndex)
{
	dTimeTrackerEvent(__FUNCTION__);
	if (m_finalized) {

		if (!NewtonBodyGetSleepState(m_body)) {

			if (m_engine) {
				m_engine->ProjectError();
			}

			for (dList<dDifferentialJoint*>::dListNode* diffNode = m_differentialList.GetFirst(); diffNode; diffNode = diffNode->GetNext()) {
				dDifferentialJoint* const diff = diffNode->GetInfo();
				diff->ProjectError();
			}

			dCustomVehicleControllerManager* const manager = (dCustomVehicleControllerManager*)GetManager();
			for (dList<dWheelJoint*>::dListNode* tireNode = m_tireList.GetFirst(); tireNode; tireNode = tireNode->GetNext()) {
				dWheelJoint* const tire = tireNode->GetInfo();
				tire->ProjectError();
				manager->Collide(tire, threadIndex);
			}
		}

#ifdef D_PLOT_ENGINE_CURVE 
		dFloat engineOmega = m_engine->GetRPM();
		dFloat tireTorque = m_engine->GetLeftSpiderGear()->m_tireTorque + m_engine->GetRightSpiderGear()->m_tireTorque;
		dFloat engineTorque = m_engine->GetLeftSpiderGear()->m_engineTorque + m_engine->GetRightSpiderGear()->m_engineTorque;
		fprintf(file_xxx, "%f, %f, %f,\n", engineOmega, engineTorque, m_engine->GetNominalTorque());
#endif
	}
}


void dCustomVehicleController::Debug(dCustomJoint::dDebugDisplay* const debugContext) const
{
//	dTrace (("Remember vehicle debug !!!\n"));
}



void dCustomVehicleController::Load(dCustomJointSaveLoad* const fileLoader) const
{
}

void dCustomVehicleController::Save(dCustomJointSaveLoad* const fileSaver) const
{
	fileSaver->Save(GetBody());

/*
	void dCustomVehicleController::dBodyPartChassis::Save(dCustomJointSaveLoad* const fileSaver) const
	{
		dFloat Ixx;
		dFloat Iyy;
		dFloat Izz;
		dFloat mass;

		NewtonBody* const body = GetBody();
		NewtonBodyGetMass(body, &mass, &Ixx, &Iyy, &Izz);

		fileSaver->SaveName("Chassis", "");
		fileSaver->SaveFloat("\taerodynamicsDownForce0", m_aerodynamicsDownForce0 / mass);
		fileSaver->SaveFloat("\taerodynamicsDownForce1", m_aerodynamicsDownForce1 / mass);
		fileSaver->SaveFloat("\taerodynamicsDownSpeedCutOff", m_aerodynamicsDownSpeedCutOff);
		fileSaver->SaveFloat("\taerodynamicsDownForceCoefficient", m_aerodynamicsDownForceCoefficient / mass);
	}
*/

/*
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;

	NewtonBody* const body = GetBody();
	NewtonBodyGetMass(body, &mass, &Ixx, &Iyy, &Izz);

	fileSaver->SaveName("vehicleSpecifications", "");
//	fileSaver->SaveName("Chassis", "");
	fileSaver->SaveFloat("\tgravityMag", m_gravityMag);
	fileSaver->SaveFloat("\tweightDistribution", m_weightDistribution);
	fileSaver->SaveFloat("\taerodynamicsDownForce0", m_aerodynamicsDownForce0 / mass);
	fileSaver->SaveFloat("\taerodynamicsDownForce1", m_aerodynamicsDownForce1 / mass);
	fileSaver->SaveFloat("\taerodynamicsDownSpeedCutOff", m_aerodynamicsDownSpeedCutOff);
	fileSaver->SaveFloat("\taerodynamicsDownForceCoefficient", m_aerodynamicsDownForceCoefficient / mass);

//	m_chassis.Save(fileSaver);
	fileSaver->SaveInt("tiresCount", m_tireList.GetCount());
	for (dList<dBodyPartTire>::dListNode* ptr = GetFirstTire(); ptr; ptr = ptr->GetNext()) {
		ptr->GetInfo().Save (fileSaver);
	} 

	fileSaver->SaveName("vehicleEnd", "");
*/
}

void dCustomVehicleController::PreUpdate(dFloat timestep, int threadIndex)
{
	dTimeTrackerEvent(__FUNCTION__);
	if (m_finalized) {
		ApplyDownForce();
		CalculateSideSlipDynamics(timestep);
		ApplySuspensionForces(timestep);

		if (m_brakesControl) {
			m_brakesControl->Update(timestep);
		}

		if (m_handBrakesControl) {
			m_handBrakesControl->Update(timestep);
		}

		if (m_steeringControl) {
			m_steeringControl->Update(timestep);
		}

		if (m_engineControl) {
			m_engineControl->Update(timestep);
		}

		if (ControlStateChanged()) {
			NewtonBodySetSleepState(m_body, 0);
		}
	}
}
