#include "globals.hlsli"
#include "emittedparticleHF.hlsli"
#include "ShaderInterop_EmittedParticle.h"

static const float3 BILLBOARD[] = {
	float3(-1, -1, 0),	// 0
	float3(1, -1, 0),	// 1
	float3(-1, 1, 0),	// 2
	float3(1, 1, 0),	// 4
};

RWStructuredBuffer<Particle> particleBuffer : register(u0);
RWStructuredBuffer<uint> aliveBuffer_CURRENT : register(u1);
RWStructuredBuffer<uint> aliveBuffer_NEW : register(u2);
RWStructuredBuffer<uint> deadBuffer : register(u3);
RWByteAddressBuffer counterBuffer : register(u4);
RWStructuredBuffer<float> distanceBuffer : register(u6);
RWByteAddressBuffer vertexBuffer_POS : register(u7);
RWByteAddressBuffer vertexBuffer_TEX : register(u8);
RWByteAddressBuffer vertexBuffer_TEX2 : register(u9);
RWByteAddressBuffer vertexBuffer_COL : register(u10);
RWStructuredBuffer<uint> culledIndirectionBuffer : register(u11);
RWStructuredBuffer<uint> culledIndirectionBuffer2 : register(u12);

#define SPH_FLOOR_COLLISION
#define SPH_BOX_COLLISION


[numthreads(THREADCOUNT_SIMULATION, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint Gid : SV_GroupIndex)
{
	//uint aliveCount = counterBuffer[0].aliveCount;
	uint aliveCount = counterBuffer.Load(PARTICLECOUNTER_OFFSET_ALIVECOUNT);

	if (DTid.x < aliveCount)
	{
		// simulation can be either fixed or variable timestep:
		const float dt = xEmitterFixedTimestep >= 0 ? xEmitterFixedTimestep : GetFrame().delta_time;

		uint particleIndex = aliveBuffer_CURRENT[DTid.x];
		Particle particle = particleBuffer[particleIndex];
		uint v0 = particleIndex * 4;

		if (particle.life > 0)
		{
			// simulate:
			for (uint i = 0; i < GetFrame().forcefieldarray_count; ++i)
			{
				ShaderEntity forceField = load_entity(GetFrame().forcefieldarray_offset + i);

				[branch]
				if (forceField.layerMask & xEmitterLayerMask)
				{
					float3 dir = forceField.position - particle.position;
					float dist;
					if (forceField.GetType() == ENTITY_TYPE_FORCEFIELD_POINT) // point-based force field
					{
						dist = length(dir);
					}
					else // planar force field
					{
						dist = dot(forceField.GetDirection(), dir);
						dir = forceField.GetDirection();
					}

					particle.force += dir * forceField.GetEnergy() * (1 - saturate(dist * forceField.GetRange())); // GetRange() is actually uploaded as 1.0 / range
				}
			}


#ifdef DEPTHCOLLISIONS
			// NOTE: We are using the textures from previous frame, so reproject against those! (previous_view_projection)

			float4 pos2D = mul(GetCamera().previous_view_projection, float4(particle.position, 1));
			pos2D.xyz /= pos2D.w;

			if (pos2D.x > -1 && pos2D.x < 1 && pos2D.y > -1 && pos2D.y < 1)
			{
				float2 uv = pos2D.xy * float2(0.5f, -0.5f) + 0.5f;
				uint2 pixel = uv * GetCamera().internal_resolution;

				float depth0 = texture_depth_history[pixel];
				float surfaceLinearDepth = compute_lineardepth(depth0);
				float surfaceThickness = 1.5f;

				float lifeLerp = 1 - particle.life / particle.maxLife;
				float particleSize = lerp(particle.sizeBeginEnd.x, particle.sizeBeginEnd.y, lifeLerp);

				// check if particle is colliding with the depth buffer, but not completely behind it:
				if ((pos2D.w + particleSize > surfaceLinearDepth) && (pos2D.w - particleSize < surfaceLinearDepth + surfaceThickness))
				{
					// Calculate surface normal and bounce off the particle:
					float depth1 = texture_depth_history[pixel + uint2(1, 0)];
					float depth2 = texture_depth_history[pixel + uint2(0, -1)];

					float3 p0 = reconstruct_position(uv, depth0, GetCamera().previous_inverse_view_projection);
					float3 p1 = reconstruct_position(uv + float2(1, 0) * GetCamera().internal_resolution_rcp, depth1, GetCamera().previous_inverse_view_projection);
					float3 p2 = reconstruct_position(uv + float2(0, -1) * GetCamera().internal_resolution_rcp, depth2, GetCamera().previous_inverse_view_projection);

					float3 surfaceNormal = normalize(cross(p2 - p0, p1 - p0));

					if (dot(particle.velocity, surfaceNormal) < 0)
					{
						particle.velocity = reflect(particle.velocity, surfaceNormal) * xEmitterRestitution;
					}
				}
			}
#endif // DEPTHCOLLISIONS

			// integrate:
			particle.force += xParticleGravity;
			particle.velocity += particle.force * dt;
			particle.position += particle.velocity * dt;

			// reset force for next frame:
			particle.force = 0;

			// drag: 
			particle.velocity *= xParticleDrag;

			float lifeLerp = 1 - particle.life / particle.maxLife;
			float particleSize = lerp(particle.sizeBeginEnd.x, particle.sizeBeginEnd.y, lifeLerp);

			[branch]
			if (xEmitterOptions & EMITTER_OPTION_BIT_SPH_ENABLED)
			{
				// debug collisions:

				float elastic = 0.6;

#ifdef SPH_FLOOR_COLLISION
				// floor collision:
				if (particle.position.y - particleSize < 0)
				{
					particle.position.y = particleSize;
					particle.velocity.y *= -elastic;
				}
#endif // FLOOR_COLLISION


#ifdef SPH_BOX_COLLISION
				// box collision:
				float3 extent = float3(40, 0, 22);
				if (particle.position.x + particleSize > extent.x)
				{
					particle.position.x = extent.x - particleSize;
					particle.velocity.x *= -elastic;
				}
				if (particle.position.x - particleSize < -extent.x)
				{
					particle.position.x = -extent.x + particleSize;
					particle.velocity.x *= -elastic;
				}
				if (particle.position.z + particleSize > extent.z)
				{
					particle.position.z = extent.z - particleSize;
					particle.velocity.z *= -elastic;
				}
				if (particle.position.z - particleSize < -extent.z)
				{
					particle.position.z = -extent.z + particleSize;
					particle.velocity.z *= -elastic;
				}
#endif // BOX_COLLISION

			}

			particle.life -= dt;

			// write back simulated particle:
			particleBuffer[particleIndex] = particle;

			// add to new alive list:
			uint newAliveIndex;
			counterBuffer.InterlockedAdd(PARTICLECOUNTER_OFFSET_ALIVECOUNT_AFTERSIMULATION, 1, newAliveIndex);
			aliveBuffer_NEW[newAliveIndex] = particleIndex;

			// Write out render buffers:
			//	These must be persistent, not culled (raytracing, surfels...)

			float opacity = saturate(lerp(1, 0, lifeLerp) * EmitterGetMaterial().baseColor.a);
			uint particleColorPacked = (particle.color_mirror & 0x00FFFFFF) | (uint(opacity * 255.0f) << 24u);

			float rotation = lifeLerp * particle.rotationalVelocity;
			float2x2 rot = float2x2(
				cos(rotation), -sin(rotation),
				sin(rotation), cos(rotation)
				);

			// Sprite sheet frame:
			const float spriteframe = xEmitterFrameRate == 0 ?
				lerp(xEmitterFrameStart, xEmitterFrameCount, lifeLerp) :
				((xEmitterFrameStart + particle.life * xEmitterFrameRate) % xEmitterFrameCount);
			const uint currentFrame = floor(spriteframe);
			const uint nextFrame = ceil(spriteframe);
			const float frameBlend = frac(spriteframe);
			uint2 offset = uint2(currentFrame % xEmitterFramesXY.x, currentFrame / xEmitterFramesXY.x);
			uint2 offset2 = uint2(nextFrame % xEmitterFramesXY.x, nextFrame / xEmitterFramesXY.x);

			for (uint vertexID = 0; vertexID < 4; ++vertexID)
			{
				// expand the point into a billboard in view space:
				float3 quadPos = BILLBOARD[vertexID];
				quadPos.x = particle.color_mirror & 0x10000000 ? -quadPos.x : quadPos.x;
				quadPos.y = particle.color_mirror & 0x20000000 ? -quadPos.y : quadPos.y;
				float2 uv = quadPos.xy * float2(0.5f, -0.5f) + 0.5f;
				float2 uv2 = uv;

				// sprite sheet UV transform:
				uv.xy += offset;
				uv.xy *= xEmitterTexMul;
				uv2.xy += offset2;
				uv2.xy *= xEmitterTexMul;

				// rotate the billboard:
				quadPos.xy = mul(quadPos.xy, rot);

				// scale the billboard:
				quadPos *= particleSize;

				// scale the billboard along view space motion vector:
				float3 velocity = mul((float3x3)GetCamera().view, particle.velocity);
				quadPos += dot(quadPos, velocity) * velocity * xParticleMotionBlurAmount;

				// rotate the billboard to face the camera:
				quadPos = mul(quadPos, (float3x3)GetCamera().view); // reversed mul for inverse camera rotation!

				// write out vertex:
				uint4 data;
				data.xyz = asuint(particle.position + quadPos);
				data.w = pack_unitvector(normalize(-GetCamera().forward));
				vertexBuffer_POS.Store4((v0 + vertexID) * 16, data);
				vertexBuffer_TEX.Store((v0 + vertexID) * 4, pack_half2(uv));
				vertexBuffer_TEX2.Store((v0 + vertexID) * 4, pack_half2(uv2));
				vertexBuffer_COL.Store((v0 + vertexID) * 4, particleColorPacked);
			}

			// Frustum culling:
			ShaderSphere sphere;
			sphere.center = particle.position;
			sphere.radius = particleSize;

			if (GetCamera().frustum.intersects(sphere))
			{
				uint prevCount;
				counterBuffer.InterlockedAdd(PARTICLECOUNTER_OFFSET_CULLEDCOUNT, 1, prevCount);

				culledIndirectionBuffer[prevCount] = prevCount;
				culledIndirectionBuffer2[prevCount] = particleIndex;

#ifdef SORTING
				// store squared distance to main camera:
				float3 eyeVector = particle.position - GetCamera().position;
				float distSQ = dot(eyeVector, eyeVector);
				distanceBuffer[prevCount] = -distSQ; // this can be negated to modify sorting order here instead of rewriting sorting shaders...
#endif // SORTING
			}

		}
		else
		{
			// kill:
			uint deadIndex;
			counterBuffer.InterlockedAdd(PARTICLECOUNTER_OFFSET_DEADCOUNT, 1, deadIndex);
			deadBuffer[deadIndex] = particleIndex;

			vertexBuffer_POS.Store4((v0 + 0) * 16, 0);
			vertexBuffer_POS.Store4((v0 + 1) * 16, 0);
			vertexBuffer_POS.Store4((v0 + 2) * 16, 0);
			vertexBuffer_POS.Store4((v0 + 3) * 16, 0);
		}
	}

}
