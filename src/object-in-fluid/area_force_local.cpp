/*
  Copyright (C) 2012,2013 The ESPResSo project
  
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

/** \file area_force_local.hpp
 *  Routines to calculate the AREA_FORCE_LOCAL energy or/and and force 
 *  for a particle triple (triangle from mesh). (Dupin2007)
 *  \ref forces.cpp
*/

#include "area_force_local.hpp"
#include "communication.hpp"


/************************************************************/

/** set parameters for the AREA_FORCE_LOCAL potential. 
*/
int area_force_local_set_params(int bond_type, double A0_l, double ka_l)
{
  if(bond_type < 0)
    return ES_ERROR;

  make_bond_type_exist(bond_type);

  bonded_ia_params[bond_type].p.area_force_local.ka_l = ka_l;
  bonded_ia_params[bond_type].p.area_force_local.A0_l = A0_l;

  bonded_ia_params[bond_type].type = BONDED_IA_AREA_FORCE_LOCAL;
  bonded_ia_params[bond_type].num = 2;				
 
  /* broadcast interaction parameters */
  mpi_bcast_ia_params(bond_type, -1); 

  return ES_OK;
}
