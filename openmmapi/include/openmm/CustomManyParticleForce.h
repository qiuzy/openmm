#ifndef OPENMM_CUSTOMTHREEBODYFORCE_H_
#define OPENMM_CUSTOMTHREEBODYFORCE_H_

/* -------------------------------------------------------------------------- *
 *                                   OpenMM                                   *
 * -------------------------------------------------------------------------- *
 * This is part of the OpenMM molecular simulation toolkit originating from   *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2008-2014 Stanford University and the Authors.      *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "Force.h"
#include "TabulatedFunction.h"
#include "internal/windowsExport.h"
#include <set>
#include <string>
#include <vector>

namespace OpenMM {

/**
 * This class supports a wide variety of nonbonded N-particle interactions, where N is user specified.  The
 * interaction energy is determined by an arbitrary, user specified algebraic expression that is evaluated for
 * every possible set of N particles in the system.  It may depend on the positions of the individual particles,
 * the distances between pairs of particles, the angles formed by sets of three particles, and the dihedral
 * angles formed by sets of four particles.
 * 
 * Be aware that the cost of evaluating an N-particle interaction increases very rapidly with N.  Values larger
 * than N=3 are rarely used.
 * 
 * We refer to a set of particles for which the energy is being evaluated  as p1, p2, p3, etc.  The energy expression
 * may depend on the following variables and functions:
 * 
 * <ul>
 * <li>x1, y1, z1, x2, y2, z2, etc.: The x, y, and z coordinates of the particle positions.  For example, x1
 * is the x coordinate of particle p1, and y3 is the y coordinate of particle p3.</li>
 * <li>distance(p1, p2): the distance between particles p1 and p2 (where "p1" and "p2" may be replaced by the names
 * of whichever particles you want to calculate the distance between).</li>
 * <li>angle(p1, p2, p3): the angle formed by the three specified particles.</li>
 * <li>dihedral(p1, p2, p3, p4): the dihedral angle formed by the four specified particles.</li>
 * <li>arbitrary global and per-particle parameters that you define.</li>
 * </ul>
 *
 * To use this class, create a CustomManyParticleForce object, passing an algebraic expression to the constructor
 * that defines the interaction energy of each set of particles.  Then call addPerParticleParameter() to define per-particle
 * parameters, and addGlobalParameter() to define global parameters.  The values of per-particle parameters are specified as
 * part of the system definition, while values of global parameters may be modified during a simulation by calling Context::setParameter().
 * 
 * Next, call addParticle() once for each particle in the System to set the values of its per-particle parameters.
 * The number of particles for which you set parameters must be exactly equal to the number of particles in the
 * System, or else an exception will be thrown when you try to create a Context.  After a particle has been added,
 * you can modify its parameters by calling setParticleParameters().  This will have no effect on Contexts that already exist
 * unless you call updateParametersInContext().
 * 
 * Multi-particle interactions can be very expensive to evaluate, so they are usually used with a cutoff distance.  If two particles
 * are further apart than the cutoff, <i>all</i> sets that include those two particles will be omitted.
 * 
 * CustomManyParticleForce also lets you specify "exclusions", particular pairs of particles whose interactions should be
 * omitted from force and energy calculations.  This is most often used for particles that are bonded to each other.
 * If you specify a pair of particles as an exclusion, <i>all</i> sets that include those two particles will be omitted.
 *
 * As an example, the following code creates a CustomManyParticleForce that implements an Axilrod-Teller potential.  This
 * is an interaction between three particles that depends on all three distances and angles formed by the particles.
 *
 * <tt><pre>CustomManyParticleForce* force = new CustomManyParticleForce(3,
 *     "C*(1+3*cos(theta1)*cos(theta2)*cos(theta3))/(r12*r13*r23)^3;"
 *     "theta1=angle(p1,p2,p3); theta2=angle(p2,p3,p1); theta3=angle(p3,p1,p2);"
 *     "r12=distance(p1,p2); r13=distance(p1,p3); r23=distance(p2,p3)");
 * <pre></tt>
 *
 * This force depends on one parameter, C.  The following code defines it as a global parameter:
 *
 * <tt><pre>
 * force->addGlobalParameter("C", 1.0);
 * </pre></tt>
 * 
 * The expression <i>must</i> be symmetric with respect to the particles.  It typically will only be evaluated once for
 * each set of particles, and no guarantee is made about which particle will be identified as "particle 1".  In the above
 * example, the energy only depends on the products cos(theta1)*cos(theta2)*cos(theta3) and r12*r13*r23, both of which are
 * unchanged if the labels p1, p2, and p3 are permuted.  If that were not true, the results would be undefined, because
 * permuting the labels would change the energy.
 * 
 * In some cases this requirement is overly restrictive.  When some particles are fundamentally different from others,
 * the expression may be inherently non-symmetric.  An example would be a water model that involves three particles,
 * two of which <i>must</i> be hydrogen and one of which <i>must</i> be oxygen.  Cases like this can be implemented
 * using particle types.
 * 
 * A particle type is an integer that you specify when you call addParticle().  (If you omit the argument, it defaults
 * to 0.)  For the water model, you could specify 0 for all oxygen atoms and 1 for all hydrogen atoms.  You can then
 * call setTypeFilter() to specify the list of allowed types for each of the N particles involved in an interaction:
 * 
 * <tt><pre>
 * set&lt;int&gt; oxygenTypes, hydrogenTypes;
 * oxygenTypes.insert(0);
 * hydrogenTypes.insert(1);
 * force->setTypeFilter(0, oxygenTypes);
 * force->setTypeFilter(1, hydrogenTypes);
 * force->setTypeFilter(2, hydrogenTypes);
 * </pre></tt>
 *
 * This specifies that of the three particles in an interaction, p1 must be oxygen while p2 and p3 must be hydrogen.
 * The energy expression will only be evaluated for triplets of particles that satisfy those requirements.  It will
 * still only be evaluated once for each triplet, so it must still be symmetric with respect to p2 and p3.
 * 
 * Expressions may involve the operators + (add), - (subtract), * (multiply), / (divide), and ^ (power), and the following
 * functions: sqrt, exp, log, sin, cos, sec, csc, tan, cot, asin, acos, atan, sinh, cosh, tanh, erf, erfc, min, max, abs, step, delta.  All trigonometric functions
 * are defined in radians, and log is the natural logarithm.  step(x) = 0 if x is less than 0, 1 otherwise.  delta(x) = 1 if x is 0, 0 otherwise.
 * The names of per-particle parameters have the suffix "1", "2", etc. appended to them to indicate the values for the multiple interacting particles.
 * For example, if you define a per-particle parameter called "charge", then the variable "charge2" is the charge of particle p2.
 * As seen above, the expression may also involve intermediate quantities that are defined following the main expression, using ";" as a separator.
 *
 * In addition, you can call addTabulatedFunction() to define a new function based on tabulated values.  You specify the function by
 * creating a TabulatedFunction object.  That function can then appear in the expression.
 */

class OPENMM_EXPORT CustomManyParticleForce : public Force {
public:
    /**
     * This is an enumeration of the different methods that may be used for handling long range nonbonded forces.
     */
    enum NonbondedMethod {
        /**
         * No cutoff is applied to nonbonded interactions.  The full set of interactions is computed exactly.
         * This necessarily means that periodic boundary conditions cannot be used.  This is the default.
         */
        NoCutoff = 0,
        /**
         * Interactions are ignored if any two particles are further apart than the cutoff distance.
         */
        CutoffNonPeriodic = 1,
        /**
         * Periodic boundary conditions are used, so that each particle interacts only with the nearest periodic copy of
         * each other particle.  Interactions are ignored if any two particles are further apart than the cutoff distance.
         */
        CutoffPeriodic = 2,
    };
    /**
     * Create a CustomManyParticleForce.
     *
     * @param particlesPerSet  the number of particles in each set for which the energy is evaluated
     * @param energy           an algebraic expression giving the interaction energy of each triplet as a function
     *                         of particle positions, inter-particle distances, angles, and any global and per-particle parameters
     */
    explicit CustomManyParticleForce(int particlesPerSet, const std::string& energy);
    ~CustomManyParticleForce();
    /**
     * Get the number of particles in each set for which the energy is evaluated
     */
    int getNumParticlesPerSet() const {
        return particlesPerSet;
    }
    /**
     * Get the number of particles for which force field parameters have been defined.
     */
    int getNumParticles() const {
        return particles.size();
    }
    /**
     * Get the number of particle pairs whose interactions should be excluded.
     */
    int getNumExclusions() const {
        return exclusions.size();
    }
    /**
     * Get the number of per-particle parameters that the interaction depends on.
     */
    int getNumPerParticleParameters() const {
        return particleParameters.size();
    }
    /**
     * Get the number of global parameters that the interaction depends on.
     */
    int getNumGlobalParameters() const {
        return globalParameters.size();
    }
    /**
     * Get the number of tabulated functions that have been defined.
     */
    int getNumTabulatedFunctions() const {
        return functions.size();
    }
    /**
     * Get the algebraic expression that gives the interaction energy of each bond
     */
    const std::string& getEnergyFunction() const;
    /**
     * Set the algebraic expression that gives the interaction energy of each bond
     */
    void setEnergyFunction(const std::string& energy);
    /**
     * Get the method used for handling long range nonbonded interactions.
     */
    NonbondedMethod getNonbondedMethod() const;
    /**
     * Set the method used for handling long range nonbonded interactions.
     */
    void setNonbondedMethod(NonbondedMethod method);
    /**
     * Get the cutoff distance (in nm) being used for nonbonded interactions.  If the NonbondedMethod in use
     * is NoCutoff, this value will have no effect.
     *
     * @return the cutoff distance, measured in nm
     */
    double getCutoffDistance() const;
    /**
     * Set the cutoff distance (in nm) being used for nonbonded interactions.  If the NonbondedMethod in use
     * is NoCutoff, this value will have no effect.
     *
     * @param distance    the cutoff distance, measured in nm
     */
    void setCutoffDistance(double distance);
    /**
     * Add a new per-particle parameter that the interaction may depend on.
     *
     * @param name     the name of the parameter
     * @return the index of the parameter that was added
     */
    int addPerParticleParameter(const std::string& name);
    /**
     * Get the name of a per-particle parameter.
     *
     * @param index     the index of the parameter for which to get the name
     * @return the parameter name
     */
    const std::string& getPerParticleParameterName(int index) const;
    /**
     * Set the name of a per-particle parameter.
     *
     * @param index          the index of the parameter for which to set the name
     * @param name           the name of the parameter
     */
    void setPerParticleParameterName(int index, const std::string& name);
    /**
     * Add a new global parameter that the interaction may depend on.
     *
     * @param name             the name of the parameter
     * @param defaultValue     the default value of the parameter
     * @return the index of the parameter that was added
     */
    int addGlobalParameter(const std::string& name, double defaultValue);
    /**
     * Get the name of a global parameter.
     *
     * @param index     the index of the parameter for which to get the name
     * @return the parameter name
     */
    const std::string& getGlobalParameterName(int index) const;
    /**
     * Set the name of a global parameter.
     *
     * @param index          the index of the parameter for which to set the name
     * @param name           the name of the parameter
     */
    void setGlobalParameterName(int index, const std::string& name);
    /**
     * Get the default value of a global parameter.
     *
     * @param index     the index of the parameter for which to get the default value
     * @return the parameter default value
     */
    double getGlobalParameterDefaultValue(int index) const;
    /**
     * Set the default value of a global parameter.
     *
     * @param index          the index of the parameter for which to set the default value
     * @param name           the default value of the parameter
     */
    void setGlobalParameterDefaultValue(int index, double defaultValue);
    /**
     * Add the nonbonded force parameters for a particle.  This should be called once for each particle
     * in the System.  When it is called for the i'th time, it specifies the parameters for the i'th particle.
     *
     * @param parameters    the list of parameters for the new particle
     * @param type          the type of the new particle
     * @return the index of the particle that was added
     */
    int addParticle(const std::vector<double>& parameters, int type=0);
    /**
     * Get the nonbonded force parameters for a particle.
     *
     * @param index       the index of the particle for which to get parameters
     * @param parameters  the list of parameters for the specified particle
     * @param type        the type of the specified particle
     */
    void getParticleParameters(int index, std::vector<double>& parameters, int& type) const;
    /**
     * Set the nonbonded force parameters for a particle.
     *
     * @param index       the index of the particle for which to set parameters
     * @param parameters  the list of parameters for the specified particle
     * @param type        the type of the specified particle
     */
    void setParticleParameters(int index, const std::vector<double>& parameters, int type);
    /**
     * Add a particle pair to the list of interactions that should be excluded.
     * 
     * In many cases, you can use createExclusionsFromBonds() rather than adding each exclusion explicitly.
     *
     * @param particle1  the index of the first particle in the pair
     * @param particle2  the index of the second particle in the pair
     * @return the index of the exclusion that was added
     */
    int addExclusion(int particle1, int particle2);
    /**
     * Get the particles in a pair whose interaction should be excluded.
     *
     * @param index      the index of the exclusion for which to get particle indices
     * @param particle1  the index of the first particle in the pair
     * @param particle2  the index of the second particle in the pair
     */
    void getExclusionParticles(int index, int& particle1, int& particle2) const;
    /**
     * Set the particles in a pair whose interaction should be excluded.
     *
     * @param index      the index of the exclusion for which to set particle indices
     * @param particle1  the index of the first particle in the pair
     * @param particle2  the index of the second particle in the pair
     */
    void setExclusionParticles(int index, int particle1, int particle2);
    /**
     * Identify exclusions based on the molecular topology.  Particles which are separated by up to a specified number of
     * bonds are added as exclusions.
     *
     * @param bonds       the set of bonds based on which to construct exclusions.  Each element specifies the indices of
     *                    two particles that are bonded to each other.
     * @param bondCutoff  pairs of particles that are separated by this many bonds or fewer are added to the list of exclusions
     */
    void createExclusionsFromBonds(const std::vector<std::pair<int, int> >& bonds, int bondCutoff);
    /**
     * Get the allowed particle types for one of the particles involved in the interaction.
     * If this an empty set (the default), no filter is applied and all interactions are evaluated
     * regardless of the type of the specified particle.
     * 
     * @param index    the index of the particle within the interaction (between 0 and getNumParticlesPerSet())
     * @param types    the allowed types for the specified particle
     */
    void getTypeFilter(int index, std::set<int>& types) const;
    /**
     * Set the allowed particle types for one of the particles involved in the interaction.
     * If this an empty set (the default), no filter is applied and all interactions are evaluated
     * regardless of the type of the specified particle.
     * 
     * @param index    the index of the particle within the interaction (between 0 and getNumParticlesPerSet())
     * @param types    the allowed types for the specified particle
     */
    void setTypeFilter(int index, const std::set<int>& types);
    /**
     * Add a tabulated function that may appear in the energy expression.
     *
     * @param name           the name of the function as it appears in expressions
     * @param function       a TabulatedFunction object defining the function.  The TabulatedFunction
     *                       should have been created on the heap with the "new" operator.  The
     *                       Force takes over ownership of it, and deletes it when the Force itself is deleted.
     * @return the index of the function that was added
     */
    int addTabulatedFunction(const std::string& name, TabulatedFunction* function);
    /**
     * Get a const reference to a tabulated function that may appear in the energy expression.
     *
     * @param index     the index of the function to get
     * @return the TabulatedFunction object defining the function
     */
    const TabulatedFunction& getTabulatedFunction(int index) const;
    /**
     * Get a reference to a tabulated function that may appear in the energy expression.
     *
     * @param index     the index of the function to get
     * @return the TabulatedFunction object defining the function
     */
    TabulatedFunction& getTabulatedFunction(int index);
    /**
     * Get the name of a tabulated function that may appear in the energy expression.
     *
     * @param index     the index of the function to get
     * @return the name of the function as it appears in expressions
     */
    const std::string& getTabulatedFunctionName(int index) const;
    /**
     * Update the per-particle parameters in a Context to match those stored in this Force object.  This method provides
     * an efficient method to update certain parameters in an existing Context without needing to reinitialize it.
     * Simply call setParticleParameters() to modify this object's parameters, then call updateParametersInContext()
     * to copy them over to the Context.
     * 
     * This method has several limitations.  The only information it updates is the values of per-particle parameters.
     * All other aspects of the Force (the energy function, nonbonded method, cutoff distance, etc.) are unaffected and can
     * only be changed by reinitializing the Context.  Also, this method cannot be used to add new particles, only to change
     * the parameters of existing ones.
     */
    void updateParametersInContext(Context& context);
protected:
    ForceImpl* createImpl() const;
private:
    class ParticleInfo;
    class ParticleParameterInfo;
    class GlobalParameterInfo;
    class ExclusionInfo;
    class FunctionInfo;
    int particlesPerSet;
    NonbondedMethod nonbondedMethod;
    double cutoffDistance;
    std::string energyExpression;
    std::vector<ParticleParameterInfo> particleParameters;
    std::vector<GlobalParameterInfo> globalParameters;
    std::vector<ParticleInfo> particles;
    std::vector<ExclusionInfo> exclusions;
    std::vector<FunctionInfo> functions;
    std::vector<std::set<int> > typeFilters;
};


/**
 * This is an internal class used to record information about a particle.
 * @private
 */
class CustomManyParticleForce::ParticleInfo {
public:
    std::vector<double> parameters;
    int type;
    ParticleInfo() {
    }
    ParticleInfo(const std::vector<double>& parameters, int type) : parameters(parameters), type(type) {
    }
};

/**
 * This is an internal class used to record information about a per-particle parameter.
 * @private
 */
class CustomManyParticleForce::ParticleParameterInfo {
public:
    std::string name;
    ParticleParameterInfo() {
    }
    ParticleParameterInfo(const std::string& name) : name(name) {
    }
};

/**
 * This is an internal class used to record information about a global parameter.
 * @private
 */
class CustomManyParticleForce::GlobalParameterInfo {
public:
    std::string name;
    double defaultValue;
    GlobalParameterInfo() {
    }
    GlobalParameterInfo(const std::string& name, double defaultValue) : name(name), defaultValue(defaultValue) {
    }
};

/**
 * This is an internal class used to record information about an exclusion.
 * @private
 */
class CustomManyParticleForce::ExclusionInfo {
public:
    int particle1, particle2;
    ExclusionInfo() {
        particle1 = particle2 = -1;
    }
    ExclusionInfo(int particle1, int particle2) :
        particle1(particle1), particle2(particle2) {
    }
};

/**
 * This is an internal class used to record information about a tabulated function.
 * @private
 */
class CustomManyParticleForce::FunctionInfo {
public:
    std::string name;
    TabulatedFunction* function;
    FunctionInfo() {
    }
    FunctionInfo(const std::string& name, TabulatedFunction* function) : name(name), function(function) {
    }
};

} // namespace OpenMM

#endif /*OPENMM_CUSTOMTHREEBODYFORCE_H_*/
