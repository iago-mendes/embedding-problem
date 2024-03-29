#include "relaxation.h"
using namespace std;

shared_ptr<Grid3DFunction> e_theta__relaxation(nullptr);
shared_ptr<Grid3DFunction> e_phi__relaxation(nullptr);
shared_ptr<Grid3DFunction> embedding__relaxation(nullptr);
shared_ptr<Metric> metric__relaxation(nullptr);

Grid *grid__relaxation;

// Free parameter
double gamma__relaxation = 1;

// Operator from equation 28 for theta
shared_ptr<Grid3DFunction> D_theta(shared_ptr<Grid3DFunction> v) {
	return v
		->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return sin(theta);}) // multiply by sin(theta)
		->partial_theta() // take theta partial derivative
		->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return 1/sin(theta);}) // divide by sin(theta)
	;
}

// Operator from equation 28 for phi
shared_ptr<Grid3DFunction> D_phi(shared_ptr<Grid3DFunction> v) {
	return v
		->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return sin(theta);}) // multiply by sin(theta)
		->partial_phi() // take theta partial derivative
		->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return 1/sin(theta);}) // divide by sin(theta)
	;
}

// Angular velocity vector from equation 30
shared_ptr<Grid3DFunction> omega() {
	shared_ptr<Grid3DFunction> commutator = get_commutator(e_theta__relaxation, e_phi__relaxation);

	return
		get_cross_product(e_phi__relaxation, D_theta(commutator))
		->added_with(get_cross_product(e_theta__relaxation, D_phi(commutator)), -1)
	;
}

// Constraint from equation 31 for theta theta
shared_ptr<GridFunction> C_theta_theta() {
	return e_theta__relaxation
		->dot_product_with(e_theta__relaxation)
		->added_with([] (double theta, double phi) {return - metric__relaxation->g_theta_theta(theta, phi);})
	;
}

// Constraint from equation 31 for theta phi
shared_ptr<GridFunction> C_theta_phi() {
	return e_theta__relaxation
		->dot_product_with(e_phi__relaxation)
		->added_with([] (double theta, double phi) {return - metric__relaxation->g_theta_phi(theta, phi);})
	;
}

// Constraint from equation 31 for phi phi
shared_ptr<GridFunction> C_phi_phi() {
	return e_phi__relaxation
		->dot_product_with(e_phi__relaxation)
		->added_with([] (double theta, double phi) {return - metric__relaxation->g_phi_phi(theta, phi);})
	;
}

// Euler stepping for theta
void update_e_theta(double time_step) {
	// From equation 42
	shared_ptr<Grid3DFunction> e_theta_derivative =
		get_cross_product(omega(), e_theta__relaxation)
		->added_with(
			e_theta__relaxation
				->multiplied_by(gamma__relaxation)
				->multiplied_by(C_theta_theta())
		, -1)
		->added_with(
			e_phi__relaxation
				->multiplied_by(gamma__relaxation)
				->multiplied_by(C_theta_phi())
				->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return 1/squared(sin(theta));})
		, -1)
	;

	e_theta__relaxation = e_theta__relaxation
		->added_with(e_theta_derivative, time_step);
		// ->added_with(e_theta_derivative->multiplied_by(time_step), [] (double theta, [[maybe_unused]] double phi) {return squared(sin(theta));});
		// ->added_with(e_theta_derivative->multiplied_by(time_step), [] (double theta, [[maybe_unused]] double phi) {return 1/sin(theta);});
}

// Euler stepping for phi
void update_e_phi(double time_step) {
	// From equation 43
	shared_ptr<Grid3DFunction> e_phi_derivative =
		get_cross_product(omega(), e_phi__relaxation)
		->added_with(
			e_theta__relaxation
				->multiplied_by(gamma__relaxation)
				->multiplied_by(C_theta_phi())
		, -1)
		->added_with(
			e_phi__relaxation
				->multiplied_by(gamma__relaxation)
				->multiplied_by(C_phi_phi())
				->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return 1/squared(sin(theta));})
		, -1)
	;

	e_phi__relaxation = e_phi__relaxation
		->added_with(e_phi_derivative, time_step);
		// ->added_with(e_phi_derivative->multiplied_by(time_step), [] (double theta, [[maybe_unused]] double phi) {return squared(sin(theta));});
		// ->added_with(e_phi_derivative->multiplied_by(time_step), [] (double theta, [[maybe_unused]] double phi) {return 1/sin(theta);});
}

// Euler stepping for embedding
// (returns embedding residual)
double update_embedding(double time_step) {
	auto cot_theta = [](double theta, [[maybe_unused]] double phi) {return 1/tan(theta);};
	auto inverse_squared_sin_theta = [](double theta, [[maybe_unused]] double phi) {return 1/squared(sin(theta));};
	auto squared_sin_theta = [](double theta, [[maybe_unused]] double phi) {return squared(sin(theta));};
	auto sqrt_sin_theta = [](double theta, [[maybe_unused]] double phi) {return sqrt(sin(theta));};

	auto laplacian =
		embedding__relaxation->second_partial_theta()
		->added_with(embedding__relaxation->partial_theta(), cot_theta)
		->added_with(embedding__relaxation->second_partial_phi(), inverse_squared_sin_theta)
	;

	auto source =
		e_theta__relaxation->partial_theta()
		->added_with(e_theta__relaxation, cot_theta)
		->added_with(e_phi__relaxation->partial_phi(), inverse_squared_sin_theta)
	;

	shared_ptr<Grid3DFunction> embedding_derivative = laplacian->added_with(source, -1);
	embedding__relaxation = embedding__relaxation->added_with(embedding_derivative->multiplied_by(squared_sin_theta), 0.5 * time_step);

	double embedding_residual = embedding_derivative->multiplied_by(sqrt_sin_theta)->norm()->rms();
	return embedding_residual;
}

double run_relaxation(
	shared_ptr<Grid3DFunction> e_theta,
	shared_ptr<Grid3DFunction> e_phi,
	shared_ptr<Grid3DFunction> embedding,
	shared_ptr<Metric> metric,
	double (*get_residual)(shared_ptr<Grid3DFunction> e_theta, shared_ptr<Grid3DFunction> e_phi),
	char *identifier,
	double final_time
) {
	e_theta__relaxation = e_theta;
	e_phi__relaxation = e_phi;
	embedding__relaxation = embedding;
	grid__relaxation = &e_theta->grid;
	metric__relaxation = metric;
	bool use_fixed_final_time = final_time != 0;

	char residuals_filename[50];
	char embedding_residuals_filename[50];
	char constraints_filename[50];
	if (identifier != nullptr) {
		sprintf(residuals_filename, "./assets/residuals_%s.csv", identifier);
		sprintf(embedding_residuals_filename, "./assets/embedding_residuals_%s.csv", identifier);
		sprintf(constraints_filename, "./assets/constraints_%s.csv", identifier);
	} else {
		sprintf(residuals_filename, "./assets/residuals_%d.csv", grid__relaxation->N_theta);
		sprintf(embedding_residuals_filename, "./assets/embedding_residuals_%d.csv", grid__relaxation->N_theta);
		sprintf(constraints_filename, "./assets/constraints.csv");
	}
	ofstream residuals_output(residuals_filename);
	ofstream embedding_residuals_output(embedding_residuals_filename);
	ofstream residual_distribution_output("./assets/residual_distribution.csv");
	ofstream constraints_output(constraints_filename);

	// output x values
	for (int i = 0; i < grid__relaxation->N_theta; i++) {
		for (int j = 0; j < grid__relaxation->N_phi; j++) {
			residual_distribution_output << grid__relaxation->theta(i);

			if (i == grid__relaxation->N_theta - 1 && j == grid__relaxation->N_phi - 1) { // last
				residual_distribution_output << endl;
			} else {
				residual_distribution_output << ",";
			}
		}
	}

	// output y values
	for (int i = 0; i < grid__relaxation->N_theta; i++) {
		for (int j = 0; j < grid__relaxation->N_phi; j++) {
			residual_distribution_output << grid__relaxation->phi(j);

			if (i == grid__relaxation->N_theta - 1 && j == grid__relaxation->N_phi - 1) { // last
				residual_distribution_output << endl;
			} else {
				residual_distribution_output << ",";
			}
		}
	}

	double time_step = 0.01; // Good for 15 x 30
	time_step *= squared(15. / (double) grid__relaxation->N_theta);
	printf("Time step = %.2e\n", time_step);

	double residual= abs(get_residual(e_theta__relaxation, e_phi__relaxation));
	double embedding_residual = INFINITY;

	Iteration best_solution;
	best_solution.solution1 = e_theta;
	best_solution.solution2 = e_phi;
	best_solution.residual = INFINITY; // Should be replaced by another solution found after relaxation is done.

	// Solve.
	int iteration_number = 0;
	int max_iterations = MAX_ITERATIONS;
	bool started_decreasing = false;
	double prev_residual = residual;
	while (
		use_fixed_final_time ? iteration_number * time_step < final_time : iteration_number < max_iterations
	) {
		if (iteration_number % OUTPUT_FREQUENCY == 0) {
			printf("(%d) Dyad residual = %e, Embedding residual = %e\n", iteration_number, residual, embedding_residual);
		}
		
		update_e_theta(time_step);
		update_e_phi(time_step);
		embedding_residual = update_embedding(time_step);

		residual = abs(get_residual(e_theta__relaxation, e_phi__relaxation));
		residuals_output << iteration_number << "," << residual << endl;

		embedding_residuals_output << iteration_number << "," << embedding_residual << endl;

		constraints_output
			<< C_theta_theta()->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return sin(theta);})->rms() << ","
			<< C_theta_phi()->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return sin(theta);})->rms() << ","
			<< C_phi_phi()->multiplied_by([] (double theta, [[maybe_unused]] double phi) {return sin(theta);})->rms() << endl;
		
		if (isnan(residual)) {
			printf("Found NaN's!\n");
		}

		if (!started_decreasing) {
			if (residual < prev_residual) {
				started_decreasing = true;
				printf("Started decreasing!\n");
			}
			prev_residual = residual;
		}

		if (started_decreasing && residual < best_solution.residual) {
			best_solution.solution1 = e_theta__relaxation;
			best_solution.solution2 = e_phi__relaxation;
			best_solution.residual = residual;
		} else if (started_decreasing && max_iterations == MAX_ITERATIONS) {
			max_iterations = iteration_number; // Stop
			printf("Reached minimum dyad residual.\n");
		}

		if (iteration_number % OUTPUT_FREQUENCY == 0) {
			// output residual norm values
			auto residual_norm = get_commutator(e_theta__relaxation, e_phi__relaxation)->norm();
			for (int i = 0; i < grid__relaxation->N_theta; i++) {
				for (int j = 0; j < grid__relaxation->N_phi; j++) {
					residual_distribution_output << residual_norm->points[i][j];

					if (i == grid__relaxation->N_theta - 1 && j == grid__relaxation->N_phi - 1) { // last
						residual_distribution_output << endl;
					} else {
						residual_distribution_output << ",";
					}
				}
			}
		}

		if (residual <= RESIDUAL_TOLERANCE) {
			// Stop after the residual tolerance is reached.
			printf("Residual tolerance was reached.\n");
			break;
		}

		iteration_number++;
	}

	int dyad_last_iteration = iteration_number;
	printf("Dyad relaxation finished with R = %.2e after %d iterations.\n", best_solution.residual, dyad_last_iteration);

	// double embedding_final_time = dyad_final_time + 50;

	max_iterations = iteration_number + 100000;
	bool printed_minimum_msg = false;
	bool printed_tolerance_msg = false;
	while (
		use_fixed_final_time ? iteration_number * time_step < 2*final_time : iteration_number < max_iterations
	) {
		if (iteration_number % OUTPUT_FREQUENCY == 0) {
			printf("(%d) Embedding residual = %e\n", iteration_number, embedding_residual);
		}
		double prev_embedding_residual = embedding_residual;

		embedding_residual = update_embedding(time_step);

		embedding_residuals_output << iteration_number << "," << embedding_residual << endl;

		if (abs(embedding_residual) > abs(prev_embedding_residual)) {
			max_iterations = iteration_number; // Stop
			if (!printed_minimum_msg) {
				printf("Reached MINIMUM embedding residual.\n");
				printed_minimum_msg = true;
			}
		}

		if ((iteration_number - dyad_last_iteration) * time_step > 10 && abs(embedding_residual) <= best_solution.residual) {
			max_iterations = iteration_number; // Stop
			if (!printed_tolerance_msg) {
				printf("Reached embedding residual TOLERANCE.\n");
				printed_tolerance_msg = true;
			}
		}

		iteration_number++;
	}
	
	printf("Embedding relaxation finished with R = %.2e after %d iterations.\n", embedding_residual, iteration_number);

	(*e_theta) = (*best_solution.solution1);
	(*e_phi) = (*best_solution.solution2);
	(*embedding) = (*embedding__relaxation);

	return best_solution.residual;
}
