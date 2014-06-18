#include <objects/VehicleObject.hpp>
#include <objects/CharacterObject.hpp>
#include <engine/GameWorld.hpp>
#include <BulletDynamics/Vehicle/btRaycastVehicle.h>
#include <sys/stat.h>
#include <data/CollisionModel.hpp>
#include <render/Model.hpp>
#include <engine/Animator.hpp>

VehicleObject::VehicleObject(GameWorld* engine, const glm::vec3& pos, const glm::quat& rot, ModelHandle* model, VehicleDataHandle data, VehicleInfoHandle info, const glm::vec3& prim, const glm::vec3& sec)
	: GameObject(engine, pos, rot, model),
	  steerAngle(0.f), throttle(0.f), brake(0.f), handbrake(false),
	  damageFlags(0), vehicle(data), info(info), colourPrimary(prim),
	  colourSecondary(sec), physBody(nullptr), physVehicle(nullptr)
{
	mHealth = 100.f;
	if(! data->modelName.empty()) {
		auto phyit = engine->gameData.collisions.find(data->modelName);
		if( phyit != engine->gameData.collisions.end()) {
			btCompoundShape* cmpShape = new btCompoundShape;
			btDefaultMotionState* msta = new btDefaultMotionState;
			msta->setWorldTransform(btTransform(
				btQuaternion(
					rot.x, rot.y, rot.z, rot.w
				),
				btVector3(
					pos.x, pos.y, pos.z
				)
			));
			CollisionModel& physInst = *phyit->second.get();

			btVector3 com(info->handling.centerOfMass.x, info->handling.centerOfMass.y, info->handling.centerOfMass.z);

			// Boxes
			for( size_t i = 0; i < physInst.boxes.size(); ++i ) {
				auto& box = physInst.boxes[i];
				auto size = (box.max - box.min) / 2.f;
				auto mid = (box.min + box.max) / 2.f;
				btCollisionShape* bshape = new btBoxShape( btVector3(size.x, size.y, size.z)  );
				btTransform t; t.setIdentity();
				t.setOrigin(btVector3(mid.x, mid.y, mid.z));
				cmpShape->addChildShape(t, bshape);
			}

			// Spheres
			for( size_t i = 0; i < physInst.spheres.size(); ++i ) {
				auto& sphere = physInst.spheres[i];
				btCollisionShape* sshape = new btSphereShape(sphere.radius/2.f);
				btTransform t; t.setIdentity();
				t.setOrigin(btVector3(sphere.center.x, sphere.center.y, sphere.center.z));
				cmpShape->addChildShape(t, sshape);
			}

			if( physInst.vertices.size() > 0 && physInst.indices.size() >= 3 ) {
				btConvexHullShape* trishape = new btConvexHullShape();
				for(size_t i = 0; i < physInst.indices.size(); ++i) {
					auto vert = physInst.vertices[physInst.indices[i]];
					trishape->addPoint({ vert.x, vert.y, vert.z });
				}
				trishape->setMargin(0.09f);
				btTransform t; t.setIdentity();
				cmpShape->addChildShape(t, trishape);
			}

			btVector3 inertia(0,0,0);
			cmpShape->calculateLocalInertia(info->handling.mass, inertia);

			btRigidBody::btRigidBodyConstructionInfo rginfo(info->handling.mass, msta, cmpShape, inertia);

			physBody = new btRigidBody(rginfo);
			physBody->setUserPointer(this);
			engine->dynamicsWorld->addRigidBody(physBody);

			physRaycaster = new VehicleRaycaster(this, engine->dynamicsWorld);
			btRaycastVehicle::btVehicleTuning tuning;

			float travel = fabs(info->handling.suspensionUpperLimit - info->handling.suspensionLowerLimit);
			tuning.m_frictionSlip = 1.8f;
			tuning.m_maxSuspensionTravelCm = travel * 100.f;

			physVehicle = new btRaycastVehicle(tuning, physBody, physRaycaster);
			physVehicle->setCoordinateSystem(0, 2, 1);
			physBody->setActivationState(DISABLE_DEACTIVATION);
			engine->dynamicsWorld->addVehicle(physVehicle);

			float kC = 0.4f;
			float kR = 0.6f;

			for(size_t w = 0; w < info->wheels.size(); ++w) {
				auto restLength = travel;
				auto heightOffset = info->handling.suspensionUpperLimit;
				btVector3 connection(
							info->wheels[w].position.x,
							info->wheels[w].position.y,
							info->wheels[w].position.z + heightOffset );
				bool front = connection.y() > 0;
				btWheelInfo& wi = physVehicle->addWheel(connection, btVector3(0.f, 0.f, -1.f), btVector3(1.f, 0.f, 0.f), restLength, data->wheelScale / 2.f, tuning, front);
				wi.m_suspensionRestLength1 = restLength;

				wi.m_maxSuspensionForce = 100000.f;
				wi.m_suspensionStiffness = (info->handling.suspensionForce * 10.f);

				//float dampEffect = (info->handling.suspensionDamping) / travel;
				//wi.m_wheelsDampingCompression = wi.m_wheelsDampingRelaxation = dampEffect;

				wi.m_wheelsDampingCompression = kC * 2.f * btSqrt(wi.m_suspensionStiffness);
				wi.m_wheelsDampingRelaxation = kR * 2.f * btSqrt(wi.m_suspensionStiffness);
				wi.m_rollInfluence = 0.0f;
				wi.m_frictionSlip = tuning.m_frictionSlip * (front ? info->handling.tractionBias : 1.f - info->handling.tractionBias);
			}

		}
	}

	// Hide all LOD and damage frames.
	animator = new Animator;
	animator->setModel(model->model);

	for(ModelFrame* f : model->model->frames) {
		auto& name = f->getName();
		bool isDam = name.find("_dam") != name.npos;
		bool isLod = name.find("lo") != name.npos;
		bool isDum = name.find("_dummy") != name.npos;
		bool isOk = name.find("_ok") != name.npos;
		if(isDam || isLod || isDum ) {
			animator->setFrameVisibility(f, false);
		}
	}

}

VehicleObject::~VehicleObject()
{
	engine->dynamicsWorld->removeRigidBody(physBody);
	engine->dynamicsWorld->removeVehicle(physVehicle);
	delete physBody;
	delete physVehicle;
	delete physRaycaster;
	delete animator;
	
	ejectAll();
}

void VehicleObject::setPosition(const glm::vec3& pos)
{
	GameObject::setPosition(pos);
	if(physBody) {
		auto t = physBody->getWorldTransform();
		t.setOrigin(btVector3(pos.x, pos.y, pos.z));
		physBody->setWorldTransform(t);
	}
}

glm::vec3 VehicleObject::getPosition() const
{
	if(physVehicle) {
		btVector3 Pos = physVehicle->getChassisWorldTransform().getOrigin();
		return glm::vec3(Pos.x(), Pos.y(), Pos.z());
	}
	return position;
}

glm::quat VehicleObject::getRotation() const
{
	if(physVehicle) {
		btQuaternion rot = physVehicle->getChassisWorldTransform().getRotation();
		return glm::quat(rot.w(), rot.x(), rot.y(), rot.z());
	}
	return rotation;
}

#include <glm/gtc/type_ptr.hpp>

void VehicleObject::tick(float dt)
{
	if(physVehicle) {
		// todo: a real engine function
		float velFac = (info->handling.maxVelocity - physVehicle->getCurrentSpeedKmHour()) / info->handling.maxVelocity;
		float engineForce = info->handling.acceleration * 150.f * throttle * velFac;

		for(int w = 0; w < physVehicle->getNumWheels(); ++w) {
			btWheelInfo& wi = physVehicle->getWheelInfo(w);
			if( info->handling.driveType == VehicleHandlingInfo::All ||
					(info->handling.driveType == VehicleHandlingInfo::Forward && wi.m_bIsFrontWheel) ||
					(info->handling.driveType == VehicleHandlingInfo::Rear && !wi.m_bIsFrontWheel))
			{
					physVehicle->applyEngineForce(engineForce, w);
			}

			float brakeReal = info->handling.brakeDeceleration * info->handling.mass * (wi.m_bIsFrontWheel? info->handling.brakeBias : 1.f - info->handling.brakeBias);
			physVehicle->setBrake(brakeReal * brake, w);

			if(wi.m_bIsFrontWheel) {
				float sign = std::signbit(steerAngle) ? -1.f : 1.f;
				physVehicle->setSteeringValue(std::min(info->handling.steeringLock*(3.141f/180.f), std::abs(steerAngle)) * sign, w);
				//physVehicle->setSteeringValue(std::min(3.141f/2.f, std::abs(steerAngle)) * sign, w);
			}
		}

		if( vehicle->type == VehicleData::BOAT ) {
			if( isInWater() ) {
				float sign = std::signbit(steerAngle) ? -1.f : 1.f;
				float steer = std::min(info->handling.steeringLock*(3.141f/180.f), std::abs(steerAngle)) * sign;
				auto orient = physBody->getOrientation();

				// Find the local-space velocity
				auto velocity = physBody->getLinearVelocity();
				velocity = velocity.rotate(-orient.getAxis(), orient.getAngle());

				// Rudder force is proportional to velocity.
				float rAngle = steer * (velFac * 0.5f + 0.5f);
				btVector3 rForce = btVector3(1000.f * velocity.y() * rAngle, 0.f, 0.f)
						.rotate(orient.getAxis(), orient.getAngle());
				btVector3 rudderPoint = btVector3(0.f, -info->handling.dimensions.y/2.f, 0.f)
						.rotate(orient.getAxis(), orient.getAngle());
				physBody->applyForce(
							rForce,
							rudderPoint);

				btVector3 rudderVector = btVector3(0.f, 1.f, 0.f)
						.rotate(orient.getAxis(), orient.getAngle());
				physBody->applyForce(
							rudderVector * engineForce * 100.f,
							rudderPoint);


				btVector3 dampforce( 10000.f * velocity.x(), velocity.y() * 100.f, 0.f );
				physBody->applyCentralForce(-dampforce.rotate(orient.getAxis(), orient.getAngle()));
			}
			physBody->setDamping(0.1f, 0.8f);
		}

		auto ws = getPosition();
		auto wX = (int) ((ws.x + WATER_WORLD_SIZE/2.f) / (WATER_WORLD_SIZE/WATER_HQ_DATA_SIZE));
		auto wY = (int) ((ws.y + WATER_WORLD_SIZE/2.f) / (WATER_WORLD_SIZE/WATER_HQ_DATA_SIZE));
		btVector3 bbmin, bbmax;
		// This is in world space.
		physBody->getAabb(bbmin, bbmax);
		float vH = bbmin.z();
		float wH = 0.f;


		if( wX >= 0 && wX < WATER_HQ_DATA_SIZE && wY >= 0 && wY < WATER_HQ_DATA_SIZE ) {
			int i = (wX*WATER_HQ_DATA_SIZE) + wY;
			int hI = engine->gameData.realWater[i];
			if( hI < NO_WATER_INDEX ) {
				wH = engine->gameData.waterHeights[hI];
				wH += engine->gameData.getWaveHeightAt(ws);
				// If the vehicle is currently underwater
				if( vH <= wH ) {
					// and was not underwater here in the last tick
					if( _lastHeight >= wH ) {
						// we are for real, underwater
						_inWater = true;
					}
					else if( _inWater == false ) {
						// It's just a tunnel or something, we good.
						_inWater = false;
					}
				}
				else {
					// The water is beneath us
					_inWater = false;
				}
			}
			else {
				_inWater = false;
			}
		}

		if( _inWater ) {
			float bbZ = info->handling.dimensions.z/2.f;

			float oZ = 0.f;
			oZ = -bbZ/2.f + (bbZ * (info->handling.percentSubmerged/120.f));

			if( vehicle->type != VehicleData::BOAT ) {
				// Damper motion
				physBody->setDamping(0.95f, 0.9f);
			}

			if( vehicle->type == VehicleData::BOAT ) {
				oZ = 0.f;
			}

			// Boats, Buoyancy offset is affected by the orientation of the chassis.
			// Vehicles, it isn't.
			glm::vec3 vFwd = glm::vec3(0.f, info->handling.dimensions.y/2.f, oZ),
					vBack = glm::vec3(0.f, -info->handling.dimensions.y/2.f, oZ);
			glm::vec3 vRt = glm::vec3( info->handling.dimensions.x/2.f, 0.f, oZ),
					vLeft = glm::vec3(-info->handling.dimensions.x/2.f, 0.f, oZ);

			vFwd = getRotation() * vFwd;
			vBack = getRotation() * vBack;
			vRt = getRotation() * vRt;
			vLeft = getRotation() * vLeft;

			// This function will try to keep v* at the water level.
			applyWaterFloat( vFwd);
			applyWaterFloat( vBack);
			applyWaterFloat( vRt);
			applyWaterFloat( vLeft);
		}
		else {
			physBody->setDamping(0.0f, 0.0f);
		}

		_lastHeight = vH;
	}
}

void VehicleObject::setSteeringAngle(float a)
{
	steerAngle = a;
}

float VehicleObject::getSteeringAngle() const
{
	return steerAngle;
}

void VehicleObject::setThrottle(float t)
{
	throttle = t;
}

float VehicleObject::getThrottle() const
{
	return throttle;
}

void VehicleObject::setBraking(float b)
{
	brake = b;
}

float VehicleObject::getBraking() const
{
	return brake;
}

void VehicleObject::setHandbraking(bool hb)
{
	handbrake = hb;
}

bool VehicleObject::getHandbraking() const
{
	return handbrake;
}

void VehicleObject::ejectAll()
{
	for(std::map<size_t, GameObject*>::iterator it = seatOccupants.begin();
		it != seatOccupants.end();
	) {
		if(it->second->type() == GameObject::Character) {
			CharacterObject* c = static_cast<CharacterObject*>(it->second);
			c->setCurrentVehicle(nullptr, 0);
			c->setPosition(getPosition());
		}
		it = seatOccupants.erase(it);
	}
}

GameObject* VehicleObject::getOccupant(size_t seat)
{
	auto it = seatOccupants.find(seat);
	if( it != seatOccupants.end() ) {
		return it->second;
	}
	return nullptr;
}

void VehicleObject::setOccupant(size_t seat, GameObject* occupant)
{
	auto it = seatOccupants.find(seat);
	if(occupant == nullptr) {
		if(it != seatOccupants.end()) {
			seatOccupants.erase(it);
		}
	}
	else {
		if(it == seatOccupants.end()) {
			seatOccupants.insert({seat, occupant});
		}
	}
}

bool VehicleObject::takeDamage(const GameObject::DamageInfo& dmg)
{
	mHealth -= dmg.hitpoints;

	const float frameDamageThreshold = 2500.f;

	if( dmg.impulse >= frameDamageThreshold ) {
		auto dpoint = dmg.damageLocation;
		dpoint -= getPosition();
		dpoint = glm::inverse(getRotation()) * dpoint;

		float d = std::numeric_limits<float>::max();
		ModelFrame* nearest = nullptr;
		// find nearest "_ok" frame.
		for(ModelFrame* f : model->model->frames) {
			auto& name = f->getName();
			if( name.find("_ok") != name.npos ) {
				auto pp = f->getMatrix() * glm::vec4(0.f, 0.f, 0.f, 1.f);
				float td = glm::distance(glm::vec3(pp), dpoint);
				if( td < d ) {
					d = td;
					nearest = f;
				}
			}
		}

		if( nearest && animator->getFrameVisibility(nearest) ) {
			animator->setFrameVisibility(nearest, false);
			// find damaged
			auto name = nearest->getName();
			name.replace(name.find("_ok"), 3, "_dam");
			auto damage = model->model->findFrame(name);
			animator->setFrameVisibility(damage, true);
		}

	}

	return true;
}

void VehicleObject::setPartDamaged(unsigned int flag, bool damaged)
{
	if(damaged) {
		damageFlags |= flag;
	}
	else {
		damageFlags = damageFlags & ~flag;
	}
}

unsigned int nameToDamageFlag(const std::string& name)
{
	if(name.find("bonnet") != name.npos) return VehicleObject::DF_Bonnet;
	if(name.find("door_lf") != name.npos) return VehicleObject::DF_Door_lf;
	if(name.find("door_rf") != name.npos) return VehicleObject::DF_Door_rf;
	if(name.find("door_lr") != name.npos) return VehicleObject::DF_Door_lr;
	if(name.find("door_rr") != name.npos) return VehicleObject::DF_Door_rr;
	if(name.find("boot") != name.npos) return VehicleObject::DF_Boot;
	if(name.find("windscreen") != name.npos) return VehicleObject::DF_Windscreen;
	if(name.find("bump_front") != name.npos) return VehicleObject::DF_Bump_front;
	if(name.find("bump_rear") != name.npos) return VehicleObject::DF_Bump_rear;
	if(name.find("wing_lf") != name.npos) return VehicleObject::DF_Wing_lf;
	if(name.find("wing_rf") != name.npos) return VehicleObject::DF_Wing_rf;
	if(name.find("wing_lr") != name.npos) return VehicleObject::DF_Wing_lr;
	if(name.find("wing_rr") != name.npos) return VehicleObject::DF_Wing_rr;
	return 0;
}

void VehicleObject::applyWaterFloat(const glm::vec3 &relPt)
{
	auto ws = getPosition() + relPt;
	auto wi = engine->gameData.getWaterIndexAt(ws);
	if(wi != NO_WATER_INDEX) {
		float h = engine->gameData.waterHeights[wi];

		// Calculate wave height
		h += engine->gameData.getWaveHeightAt(ws);

		if ( ws.z <= h ) {
			float x = (h - ws.z);
			float F = WATER_BUOYANCY_K * x + -WATER_BUOYANCY_C * physBody->getLinearVelocity().z();
			physBody->applyForce(btVector3(0.f, 0.f, F),
								 btVector3(relPt.x, relPt.y, relPt.z));
		}
	}
}

// Dammnit Bullet

class ClosestNotMeRayResultCallback : public btCollisionWorld::ClosestRayResultCallback
{
	btCollisionObject* _self;
public:

	ClosestNotMeRayResultCallback( btCollisionObject* self, const btVector3& from, const btVector3& to )
		: ClosestRayResultCallback( from, to ), _self( self ) {}

	virtual btScalar addSingleResult(btCollisionWorld::LocalRayResult &rayResult, bool normalInWorldSpace)
	{
		if( rayResult.m_collisionObject == _self ) {
			return 1.0;
		}
		return ClosestRayResultCallback::addSingleResult( rayResult, normalInWorldSpace );
	}
};

void *VehicleRaycaster::castRay(const btVector3 &from, const btVector3 &to, btVehicleRaycaster::btVehicleRaycasterResult &result)
{
	ClosestNotMeRayResultCallback rayCallback( _vehicle->physBody, from, to );

	_world->rayTest(from, to, rayCallback);

	if( rayCallback.hasHit() ) {
		const btRigidBody* body = btRigidBody::upcast( rayCallback.m_collisionObject );

		if( body && body->hasContactResponse() ) {
			result.m_hitPointInWorld = rayCallback.m_hitPointWorld;
			result.m_hitNormalInWorld = rayCallback.m_hitNormalWorld;
			result.m_hitNormalInWorld.normalize();
			result.m_distFraction = rayCallback.m_closestHitFraction;
			return (void*) body;
		}
	}
	return 0;
}