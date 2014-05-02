/*
  Copyright (C) 2011,2012,2013 The ESPResSo project
  
  This file is part of ESPResSo.
  
  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>. 
*/

#include "collision.hpp"
#include "cells.hpp"
#include "communication.hpp"
#include "errorhandling.hpp"
#include "grid.hpp"

#ifdef COLLISION_DETECTION

/// Data type holding the info about a single collision
typedef struct {
  int pp1; // 1st particle id
  int pp2; // 2nd particle id
  double point_of_collision[3]; 
} collision_struct;

// During force calculation, colliding particles are recorded in thequeue
// The queue is processed after force calculation, when it is save to add
// particles
static collision_struct *collision_queue;
// Number of collisions recoreded in the queue
static int number_of_collisions;
// distance between two particles
static double dist_betw_part, vec21[3]; 

/// Parameters for collision detection
Collision_parameters collision_params = { 0, };

int collision_detection_set_params(int mode, double d, int bond_centers, int bond_vs,int t, int bond_three_particles, int angle_resolution)

{
  if (mode & COLLISION_MODE_VS)
    mode |= COLLISION_MODE_BOND;

  // If we don't have virtual sites, virtual site binding isn't possible.
#ifndef VIRTUAL_SITES_RELATIVE
  if (mode & COLLISION_MODE_VS)
    return 1;
#endif

#ifndef COLLISION_USE_BROKEN_PARALLELIZATION
  // Binding so far only works on a single cpu
  if (mode && n_nodes != 1)
    return 2;
#endif

  // Check if bonded ia exist
  if ((mode & COLLISION_MODE_BOND) &&
      (bond_centers >= n_bonded_ia))
    return 3;
  if ((mode & COLLISION_MODE_VS) &&
      (bond_vs >= n_bonded_ia))
    return 3;
  
  if ((mode & COLLISION_MODE_BOND) &&
      (bonded_ia_params[bond_centers].num != 1))
    return 4;
  
  if ((mode & COLLISION_MODE_VS) && !(bonded_ia_params[bond_vs].num == 1 ||
				      bonded_ia_params[bond_vs].num == 2))
    return 5;

  /* Gizem: Check all the bonds and angles. BUT, bond_three_particles is 2, and 
  three_particle_angle_resolution changes from 0 to 180. I do not understand how to do this */

  
  for (int i=collision_params.bond_three_particles;1<collision_params.bond_three_particles+collision_params.three_particle_angle_resolution;i++)
  {
  // in the check code below, I need to use "i" right?
  if ((mode & COLLISION_MODE_BIND_THREE_PARTICLES) && !(bonded_ia_params[bond_centers].num == 1 &&
				      		        bonded_ia_params[bond_three_particles].num + collision_params.three_particle_angle_resolution == i))
    return 6;
  }
  // Set params
  collision_params.mode=mode;
  collision_params.bond_centers=bond_centers;
  collision_params.bond_vs=bond_vs;
  collision_params.distance=d;
  collision_params.vs_particle_type=t;
  collision_params.bond_three_particles=bond_three_particles;
  collision_params.three_particle_angle_resolution=angle_resolution;

  make_particle_type_exist(t);

  mpi_bcast_collision_params();

  return 0;
}

// Detect a collision between the given particles.
// Add it to the queue in case virtual sites should be added at the point of collision
void detect_collision(Particle* p1, Particle* p2)
{
  // The check, whether collision detection is actually turned on is performed in forces.hpp

  //double dist_betw_part, vec21[3]; 
  int part1, part2, size;

  // Obtain distance between particles
  dist_betw_part = sqrt(distance2vec(p1->r.p, p2->r.p, vec21));
  if (dist_betw_part > collision_params.distance)
    return;

  part1 = p1->p.identity;
  part2 = p2->p.identity;
      
  // Retrieving the particles from local_particles is necessary, because the particle might be a
  // ghost, and those don't contain bonding info
  p1 = local_particles[part1];
  p2 = local_particles[part2];

#ifdef COLLISION_USE_BROKEN_PARALLELIZATION
  // Ignore particles too distant to be on the same processor
  if (!p1 || !p2)
    return; 
#endif

#ifdef VIRTUAL_SITES_RELATIVE
  // Ignore virtual particles
  if ((p1->p.isVirtual) || (p2->p.isVirtual))
    return;
#endif

  // Check, if there's already a bond between the particles
  // First check the bonds of p1
  if (p1->bl.e) {
    int i = 0;
    while(i < p1->bl.n) {
      size = bonded_ia_params[p1->bl.e[i]].num;
      
      if (p1->bl.e[i] == collision_params.bond_centers &&
          p1->bl.e[i + 1] == part2) {
        // There's a bond, already. Nothing to do for these particles
        return;
      }
      i += size + 1;
    }
  }
  if (p2->bl.e) {
    // Check, if a bond is already stored in p2
    int i = 0;
    while(i < p2->bl.n) {
      size = bonded_ia_params[p2->bl.e[i]].num;

      /* COMPARE P2 WITH P1'S BONDED PARTICLES*/

      if (p2->bl.e[i] == collision_params.bond_centers &&
          p2->bl.e[i + 1] == part1) {
        return;
      }
      i += size + 1;
    }
  }

  /* If we're still here, there is no previous bond between the particles,
     we have a new collision */

  /* create marking bond between the colliding particles immediately */

  if (collision_params.mode & COLLISION_MODE_BOND)
  {
    int bondG[2];
    int primary = part1, secondary = part2;
#ifdef COLLISION_USE_BROKEN_PARALLELIZATION
    // put the bond to the physical particle; at least one partner always is
    if (p1->l.ghost) {
      primary = part2;
      secondary = part1;
    }
#endif
    bondG[0]=collision_params.bond_centers;
    bondG[1]=secondary;
    local_change_bond(primary, bondG, 0);
  }


  if (collision_params.mode & (COLLISION_MODE_VS | COLLISION_MODE_EXCEPTION)) {
    /* If we also create virtual sites or throw an exception, we add the collision
       to the queue to process later */

    // Point of collision
    double new_position[3];
    for (int i=0;i<3;i++) {
      new_position[i] = p1->r.p[i] - vec21[i] * 0.50;
    }
       
    number_of_collisions++;

    // Allocate mem for the new collision info
    collision_queue = (collision_struct *) realloc (collision_queue, (number_of_collisions) * sizeof(collision_struct));
      
    // Save the collision      
    collision_queue[number_of_collisions-1].pp1 = part1;
    collision_queue[number_of_collisions-1].pp2 = part2;
    for (int i=0;i<3;i++) {
      collision_queue[number_of_collisions-1].point_of_collision[i] = new_position[i]; 
    }
  }
}

void prepare_collision_queue()
{
  
  number_of_collisions=0;

  collision_queue = (collision_struct *) malloc (sizeof(collision_struct));

}

// Handle the collisions stored in the queue
void handle_collisions ()
{
   //printf("number of collisions in handle collision are %d\n",number_of_collisions);

  double cosine, vec1[3], vec2[3],  d1i, d2i, dist2;
  int j;

  for (int i = 0; i < number_of_collisions; i++) {
      printf("Handling collision of particles %d %d\n", collision_queue[i].pp1, collision_queue[i].pp2);
    //  fflush(stdout);

    if (collision_params.mode & (COLLISION_MODE_EXCEPTION)) {

      // if desired, raise a runtime exception (background error) on collision
      char *exceptiontxt = runtime_error(128 + 2*ES_INTEGER_SPACE);
      int id1, id2;
      if (collision_queue[i].pp1 > collision_queue[i].pp2) {
	id1 = collision_queue[i].pp2;
	id2 = collision_queue[i].pp1;
      }
      else {
	id1 = collision_queue[i].pp1;
	id2 = collision_queue[i].pp2;
      }

      ERROR_SPRINTF(exceptiontxt, "{collision between particles %d and %d} ",
		    id1, id2);
    }

    /* If we don't have virtual_sites_relative, only bonds between centers of 
       colliding particles are possible and nothing is to be done here */
#ifdef VIRTUAL_SITES_RELATIVE
    if (collision_params.mode & COLLISION_MODE_VS) {

      // add virtual sites placed at the point of collision and bind them
      int bondG[3];
  
      // Virtual site related to first particle in the collision
      place_particle(max_seen_particle+1,collision_queue[i].point_of_collision);
      vs_relate_to(max_seen_particle,collision_queue[i].pp1);
      (local_particles[max_seen_particle])->p.isVirtual=1;
      (local_particles[max_seen_particle])->p.type=collision_params.vs_particle_type;
  
      // Virtual particle related to 2nd particle of the collision
      place_particle(max_seen_particle+1,collision_queue[i].point_of_collision);
      vs_relate_to(max_seen_particle,collision_queue[i].pp2);
      (local_particles[max_seen_particle])->p.isVirtual=1;
      (local_particles[max_seen_particle])->p.type=collision_params.vs_particle_type;
  
      switch (bonded_ia_params[collision_params.bond_vs].num) {
      case 1: {
	// Create bond between the virtual particles
	bondG[0] = collision_params.bond_vs;
	bondG[1] = max_seen_particle-1;
	local_change_bond(max_seen_particle, bondG, 0);
	break;
      }
      case 2: {
	// Create 1st bond between the virtual particles
	bondG[0] = collision_params.bond_vs;
	bondG[1] = collision_queue[i].pp1;
	bondG[2] = collision_queue[i].pp2;
	local_change_bond(max_seen_particle,   bondG, 0);
	local_change_bond(max_seen_particle-1, bondG, 0);
	break;
      }
      }
    }
#endif

  }

  // three-particle-binding part

  if (collision_params.mode & (COLLISION_MODE_BIND_THREE_PARTICLES)) {  
     Cell *cell;
     Particle *p, *p1, *p2;
     int size, idp, idp1, idp2, bondT[3]; 
     double phi;
     // first iterate over cells, get one of the cells and find how many particles in this cell
     for (int c=0; c<local_cells.n; c++) {
         cell=local_cells.cell[c];
         // iterate over particles in the cell
         for (int a=0; a<cell->n; a++) {
             p=&cell->part[a];
             idp=p->p.identity;
             // for all p:
             for (int ij=0; ij<number_of_collisions; ij++) {
                 p1=local_particles[collision_queue[ij].pp1];
                 p2=local_particles[collision_queue[ij].pp2];
                 idp1=p1->p.identity;
                 idp2=p2->p.identity;
                 // Obtain distance between particles
                 dist_betw_part = sqrt(distance2vec(p->r.p, p1->r.p, vec21));
                 if (dist_betw_part < collision_params.distance) {
                    // Check three-particle-bond on p1
                    if (p1->bl.e) {
                       int b = 0;
                       while (b < p1->bl.n) {
                         size = bonded_ia_params[p1->bl.e[b]].num;
                         // Check three-particle-binding
/*RUDOLF: here, i assumed that the variable "size" should give 2, for angular potential bonnd. 
I could not find this anywhere. but, i guess, when we talked about the implementation of the 
single bond, axel or you told me sth like this. but i am not sure.*/
                         if (size=2) {
                           for (int count=collision_params.bond_three_particles; count<collision_params.bond_three_particles+collision_params.three_particle_angle_resolution-1; count++) {
                            if (p1->bl.e[b] == count && ((p1->bl.e[b + 1] == idp && p1->bl.e[b + 2] == idp2) || (p1->bl.e[b + 1] == idp2 && p1->bl.e[b + 2] == idp) )) {
                               // There's a bond, already. Nothing to do for these particles
                               return;
                            }
                           }
                         }
                         b += size + 1;
                       }
                    }
                    // If we are still here, we need to create angular bond
                    // First, find the angle between the particle p, p1 and p2
                    cosine=0.0;
                    /* vector from p_mid to p_left */
                    get_mi_vector(vec1, p->r.p, p1->r.p);
                    dist2 = sqrlen(vec1);
                    d1i = 1.0 / sqrt(dist2);
                    for(j=0;j<3;j++) vec1[j] *= d1i;
                    /* vector from p_right to p_mid */
                    get_mi_vector(vec2, p1->r.p, p2->r.p);
                    dist2 = sqrlen(vec2);
                    d2i = 1.0 / sqrt(dist2);
                    for(j=0;j<3;j++) vec2[j] *= d2i;
                    /* scalar produvt of vec1 and vec2 */
                    cosine = scalar(vec1, vec2);
                    if ( cosine >  TINY_COS_VALUE)  cosine = TINY_COS_VALUE;
                    if ( cosine < -TINY_COS_VALUE)  cosine = -TINY_COS_VALUE;
                    /* bond angle */
                    phi =  acos(-cosine);
                    double bond_angle, bond_id;
                    bond_angle=phi*57.2957795;

                    if (phi=0) bond_id=collision_params.bond_three_particles;
                    if (phi=PI) bond_id=collision_params.bond_three_particles+collision_params.three_particle_angle_resolution-1;
                    bond_id=floor(bond_angle);
                    
                    bondT[0] = bond_id;
	            bondT[1] = p->p.identity;
	            bondT[2] = collision_queue[ij].pp2;
	            local_change_bond(collision_queue[ij].pp1,   bondT, 0);
                   
                 }
                 // same between particle p and p2      
                 dist_betw_part = sqrt(distance2vec(p->r.p, p2->r.p, vec21));
                 if (dist_betw_part < collision_params.distance) {
                    // Check three-particle-bond on p1
                    if (p2->bl.e) {
                       int b = 0;
                       while (b < p2->bl.n) {
                         size = bonded_ia_params[p2->bl.e[b]].num;
                         // Check three-particle-binding
                         if (size=2) {
                           for (int count=collision_params.bond_three_particles; count<collision_params.bond_three_particles+collision_params.three_particle_angle_resolution-1; count++) {
                            if (p2->bl.e[b] == count && ((p2->bl.e[b + 1] == idp && p2->bl.e[b + 2] == idp1) || (p2->bl.e[b + 1] == idp1 && p2->bl.e[b + 2] == idp) )) {
                               // There's a bond, already. Nothing to do for these particles
                               return;
                            }
                           }
                         }
                         b += size + 1;
                       }
                    }
                    // If we are still here, we need to create angular bond
                    // First, find the angle between the particle p, p1 and p2
                    cosine=0.0;
                    /* vector from p_mid to p_left */
                    get_mi_vector(vec1, p->r.p, p2->r.p);
                    dist2 = sqrlen(vec1);
                    d1i = 1.0 / sqrt(dist2);
                    for(j=0;j<3;j++) vec1[j] *= d1i;
                    /* vector from p_right to p_mid */
                    get_mi_vector(vec2, p2->r.p, p1->r.p);
                    dist2 = sqrlen(vec2);
                    d2i = 1.0 / sqrt(dist2);
                    for(j=0;j<3;j++) vec2[j] *= d2i;
                    /* scalar produvt of vec1 and vec2 */
                    cosine = scalar(vec1, vec2);
                    if ( cosine >  TINY_COS_VALUE)  cosine = TINY_COS_VALUE;
                    if ( cosine < -TINY_COS_VALUE)  cosine = -TINY_COS_VALUE;
                    /* bond angle */
                    phi =  acos(-cosine);
                    double bond_angle, bond_id;
                    bond_angle=phi*57.2957795;

                    if (phi=0) bond_id=collision_params.bond_three_particles;
                    if (phi=PI) bond_id=collision_params.bond_three_particles+collision_params.three_particle_angle_resolution-1;
                    bond_id=floor(bond_angle);
                    
                    bondT[0] = bond_id;
	            bondT[1] = p->p.identity;
	            bondT[2] = collision_queue[ij].pp1;
	            local_change_bond(collision_queue[ij].pp2,   bondT, 0);
                   
                 }
             }
         }
     }

  }

  // Reset the collision queue
  number_of_collisions = 0;

  free(collision_queue);

  announce_resort_particles();
}

#endif
