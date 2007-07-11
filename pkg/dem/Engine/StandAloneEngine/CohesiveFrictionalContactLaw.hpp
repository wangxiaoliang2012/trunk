/*************************************************************************
*  Copyright (C) 2007 by Bruno Chareyre                                 *
*  bruno.chareyre@imag.fr                                               *
*                                                                        *
*  This program is free software; it is licensed under the terms of the  *
*  GNU General Public License v2 or later. See file LICENSE for details. *
*************************************************************************/

#ifndef COHESIVE_FRICTIONAL_CONTACT_LAW_HPP
#define COHESIVE_FRICTIONAL_CONTACT_LAW_HPP

#include<yade/core/InteractionSolver.hpp>

#include <set>
#include <boost/tuple/tuple.hpp>

class PhysicalAction;

class CohesiveFrictionalContactLaw : public InteractionSolver
{
/// Attributes
	private :
		shared_ptr<PhysicalAction> actionForce;
		shared_ptr<PhysicalAction> actionMomentum;

	public :
		int sdecGroupMask;
		bool momentRotationLaw;
	
		CohesiveFrictionalContactLaw();
		void action(Body* body);

	protected :
		void registerAttributes();
	REGISTER_CLASS_NAME(CohesiveFrictionalContactLaw);
	REGISTER_BASE_CLASS_NAME(InteractionSolver);
};

REGISTER_SERIALIZABLE(CohesiveFrictionalContactLaw,false);

#endif // COHESIVE_FRICTIONAL_CONTACT_LAW_HPP
