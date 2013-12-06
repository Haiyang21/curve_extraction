// Petter Strandmark and Johannes Ulén 2013.
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <limits>

#include <spii/auto_diff_term.h>
#include <spii/constraints.h>
#include <spii/solver.h>

// Mex
#include "curve_segmentation.h"

using namespace spii;
using namespace curve_extraction;

#ifdef USE_OPENMP
#include <omp.h>
double get_wtime()
{
	return ::omp_get_wtime();
}
#else
#include <ctime>
double get_wtime()
{
	return std::time(0);
}
#endif

void mex_log_function(const std::string& str)
{
	mexPrintf("%s\n", str.c_str());
}

struct Point
{
	double xyz[3];
};

class Length
{
	public:
		Length(std::vector<double> dims_, double penalty_)
		{
			dims = dims_;
			penalty = penalty_;
		}

		template<typename R>
		R operator()(const R* const point1, const R* const point2) const
		{
			using std::sqrt;
			R dx = dims[0]*(point1[0] - point2[0]);
			R dy = dims[1]*(point1[1] - point2[1]);
			R dz = dims[2]*(point1[2] - point2[2]);

			return  penalty*sqrt(dx*dx + dy*dy + dz*dz);
		}

	private:
		std::vector<double> dims;
		double penalty;
};

class Curvature
{
public:
	Curvature(std::vector<double> dims_, double penalty_,  double power_)
	{
		dims = dims_;
		power = power_;
		penalty = penalty_;
	}

	template<typename R>
	R operator()(const R* const point1, const R* const point2, const R* const point3) const
	{
		return  penalty * compute_curvature<R>(
						point1[0]*dims[0], point1[1]*dims[1], point1[2]*dims[2],
						point2[0]*dims[0], point2[1]*dims[1], point2[2]*dims[2],
						point3[0]*dims[0], point3[1]*dims[1], point3[2]*dims[2],
						power);
	}

private:
	std::vector<double> dims;
	double power;
	double penalty;
};

class Torsion
{
public:
	Torsion(std::vector<double> dims_, double penalty_,  double power_)
	{
		dims = dims_;
		power = power_;
		penalty = penalty_;
	}

	template<typename R>
	R operator()(const R* const point1,
	             const R* const point2,
	             const R* const point3,
	             const R* const point4) const
	{
		return  penalty * compute_torsion<R>(
						point1[0]*dims[0], point1[1]*dims[1], point1[2]*dims[2],
						point2[0]*dims[0], point2[1]*dims[1], point2[2]*dims[2],
						point3[0]*dims[0], point3[1]*dims[1], point3[2]*dims[2],
						point4[0]*dims[0], point4[1]*dims[1], point4[2]*dims[2],
						power);
	}

private:
	std::vector<double> dims;
	double power;
	double penalty;
};

template<typename DataTermImpl>
class LinearUnary
{
public:
	LinearUnary(DataTermImpl& data_term_)
		: data_term(data_term_)
	{ }

	template<typename R>
	R operator()(const R* const point1, const R* const point2) const
	{
		return data_term.evaluate_line_integral(point1[0],point1[1],point1[2],
		                                        point2[0],point2[1],point2[2]);
	}

private:
	DataTermImpl& data_term;
};


void mexFunction_main(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
	double start_time = ::get_wtime();

	using namespace std;
	ASSERT(nrhs == 3);

	const matrix<double> unary_matrix(prhs[0]);
	const matrix<double> path(prhs[1]);

	MexParams params(1, prhs+2);
	vector<double> voxeldimensions = params.get< vector<double> >("voxeldimensions");
	InstanceSettings settings = parse_settings(params);

	if (voxeldimensions.empty())
	{
		voxeldimensions.push_back(1.0);
		voxeldimensions.push_back(1.0);
		voxeldimensions.push_back(1.0);
	}

	const double function_improvement_tolerance = params.get<double>("function_improvement_tolerance", 1e-12);
	const double argument_improvement_tolerance = params.get<double>("argument_improvement_tolerance", 1e-12);
	const int maxiter = params.get<double>("maxiter", 1000);

	string str_unary_type = params.get<string>("unary_type", "linear");
	enum Unary_type {trilinear, linear};
	Unary_type unary_type;

	if (str_unary_type == "linear")
		unary_type = linear;
	else if  (str_unary_type == "trilinear")
		unary_type = trilinear;
	else
		throw runtime_error("Unknown unary type");

	string str_solver_method = params.get<string>("local_solver","lbfgs");
	enum Solver_method {newton, lbfgs, nelder_mead};
	Solver_method solver_method;

	if (str_solver_method == "newton")
		solver_method = newton;
	else if (str_solver_method == "lbfgs")
		solver_method = lbfgs;
	else if (str_solver_method == "nelder_mead")
		solver_method = nelder_mead;
	else
		throw runtime_error("Unknown Solver");


	string str_factorization_method = params.get<string>("factorization_method", "iterative");
	enum Factorization_method {bkp, iterative};
	Factorization_method factorization_method;

	if (str_factorization_method == "iterative")
		factorization_method = iterative;
	else if (str_factorization_method == "bkp")
		factorization_method = bkp;
	else
		throw runtime_error("Unknown factorization method");


	const int n = path.M;
	const int dim = path.N;

	ASSERT( (dim == 2) || (dim == 3) );

	if (settings.VERBOSE)
	{
		mexPrintf("Solving using : %s \n", str_solver_method.c_str());
		mexPrintf("Maximum iterations: %d \n", maxiter);
		mexPrintf("function_improvement_tolerance: %g \n", function_improvement_tolerance);
		mexPrintf("argument_improvement_tolerance: %g \n", argument_improvement_tolerance);
		mexPrintf("Factorization_method : %s \n", str_factorization_method.c_str());
	}

	// Function to be optimized
	Function f;

	// Create the points and add them as variables
	// to the function.
	vector<Point> points(n);
	vector<double> lower_bound(3, 0.0);
	vector<double> upper_bound(3, 0.0);
	upper_bound[0] = unary_matrix.M - 1;
	upper_bound[1] = unary_matrix.N - 1;
	upper_bound[2] = unary_matrix.O - 1;
	if (unary_matrix.O == 1) {
		lower_bound[2] = -1;
		upper_bound[2] = +1;	
	}

	for (int i = 0; i < n; i++)
	{
		// Zero based index
		points[i].xyz[0] = path(i,0) - 1;
		points[i].xyz[1] = path(i,1) - 1;
		points[i].xyz[2] = path(i,2) - 1;

		const double magic_one_offset = 0.1;

		for (int j = 0; j < 3; ++j) {
			points[i].xyz[j] = max(points[i].xyz[j], magic_one_offset);
		}


		points[i].xyz[0] = min(points[i].xyz[0], unary_matrix.M - 1.0 - magic_one_offset);
		points[i].xyz[1] = min(points[i].xyz[1], unary_matrix.N - 1.0 - magic_one_offset);
		points[i].xyz[2] = min(points[i].xyz[2], unary_matrix.O - 1.0 - magic_one_offset);

		f.add_variable_with_change<spii::Box>(points[i].xyz, 3, 3, &lower_bound[0], &upper_bound[0]);
	}

	// The start and end of the curve is fixed.
	f.set_constant(points[0].xyz, true);
	f.set_constant(points[1].xyz, true);

	f.set_constant(points[n-2].xyz, true);
	f.set_constant(points[n-1].xyz, true);

	PieceWiseConstant data_term( unary_matrix.data,
	                             unary_matrix.M,
	                             unary_matrix.N,
	                             unary_matrix.O,
	                             voxeldimensions);

	// Adding unary cost
	if (unary_type == linear)
	{
		auto unary = std::make_shared<AutoDiffTerm<LinearUnary<PieceWiseConstant>, 3, 3>>(data_term);

		for (int i = 1; i < n; ++i)
			f.add_term(unary, points[i-1].xyz, points[i].xyz);
	}
	else if (unary_type == trilinear)
	{
		mexErrMsgTxt("Trilinear not supported.");
	}
	// Functor for each type of regularization penalty
	auto length = std::make_shared<AutoDiffTerm<Length, 3, 3>>(voxeldimensions, settings.length_penalty);
	auto curvature = std::make_shared<AutoDiffTerm<Curvature, 3, 3, 3>>
				(voxeldimensions, settings.curvature_penalty, settings.curvature_power);
	auto torsion = std::make_shared<AutoDiffTerm<Torsion, 3, 3, 3, 3>>
				(voxeldimensions, settings.torsion_penalty, settings.torsion_power);

	// Adding length cost
	if (settings.length_penalty > 0)
	{
		for (int i = 1; i < n; ++i)
			f.add_term(length, points[i-1].xyz, points[i].xyz);
	}

	// Add curvature cost
	if (settings.curvature_penalty > 0)
	{
		for (int i = 2; i < n; ++i)
		{
			vector<double*> args;
			args.push_back(points[i-2].xyz);
			args.push_back(points[i-1].xyz);
			args.push_back(points[i].xyz);

			f.add_term(curvature, args);
		}
	}

	// Adding torsion cost
	if (settings.torsion_penalty > 0)
	{
		for (int i = 3; i < n; ++i)
		{
			vector<double*> args;
			args.push_back(points[i-3].xyz);
			args.push_back(points[i-2].xyz);
			args.push_back(points[i-1].xyz);
			args.push_back(points[i].xyz);

			f.add_term(torsion, args);
		}
	}
	if (settings.VERBOSE)
		mexPrintf("Initial function value: %.3e\n", f.evaluate());


	std::unique_ptr<Solver> solver;

	if (solver_method == newton) {
		solver.reset(new NewtonSolver);
		NewtonSolver* newton_solver = dynamic_cast<NewtonSolver*>(solver.get());

		if (factorization_method == iterative)
			newton_solver->factorization_method = NewtonSolver::ITERATIVE;
		else if (factorization_method == bkp)
			newton_solver->factorization_method = NewtonSolver::BKP;
		else
			mexErrMsgTxt("Unknown factorization method.");
	}
	else if (solver_method == lbfgs) {
		solver.reset(new LBFGSSolver);
	}
	else if (solver_method == nelder_mead){
		//solver.solve_nelder_mead(f, &results);
		mexErrMsgTxt("NM not upported.");
	}
	else {
		mexErrMsgTxt("Unknown method.");
	}

	SolverResults results;

	solver->log_function = mex_log_function;

	solver->maximum_iterations = maxiter;
	solver->function_improvement_tolerance = function_improvement_tolerance;
	solver->argument_improvement_tolerance = argument_improvement_tolerance;

	f.set_number_of_threads(1);

	solver->solve(f, &results);

	if (settings.VERBOSE)
	{
		std::stringstream sout;
		sout << results << endl;
		mexPrintf("%s\n", sout.str().c_str());
		mexPrintf("Final function value:   %.3e\n", f.evaluate());
	}


	matrix<double> resulting_path(points.size(), dim);
	for (int i = 0; i <n; i++)
	{
		resulting_path(i,0) = points[i].xyz[0] + 1;
		resulting_path(i,1) = points[i].xyz[1] + 1;
		resulting_path(i,2) = points[i].xyz[2] + 1;
	}

	plhs[0] = resulting_path;

	// Info from solver
	matrix<double> o_time(1);
	matrix<double> o_cost(1);

	double end_time = ::get_wtime();

	o_cost(0) = f.evaluate();
	o_time(0) = end_time - start_time;

	// Solution info
	const int num_fields = 2;
	const char *fieldnames[num_fields] = {"cost", "time"};
	plhs[1]  =  mxCreateStructMatrix(1 , 1 , num_fields , fieldnames);
	mxSetFieldByNumber(plhs[1] , 0 , 0 , o_cost);
	mxSetFieldByNumber(plhs[1] , 0 , 1 , o_time);
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
	try {
		mexFunction_main(nlhs, plhs, nrhs, prhs);
	}
	catch (std::exception& error) {
		mexErrMsgTxt(error.what());
	}
	catch (...) {
		mexErrMsgTxt("Unknown exception.");
	}
}