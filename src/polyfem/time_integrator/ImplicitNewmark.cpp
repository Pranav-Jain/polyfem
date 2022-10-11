#include "ImplicitNewmark.hpp"

namespace polyfem::time_integrator
{
	void ImplicitNewmark::set_parameters(const json &params)
	{
		beta_ = params.at("gamma");
		gamma_ = params.at("beta");
	}

	void ImplicitNewmark::update_quantities(const Eigen::VectorXd &x)
	{
		const Eigen::VectorXd v = compute_velocity(x);
		set_a_prev(compute_acceleration(v));
		set_v_prev(v);
		set_x_prev(x);
	}

	Eigen::VectorXd ImplicitNewmark::x_tilde() const
	{
		return x_prev() + dt() * (v_prev() + dt() * (0.5 - beta()) * a_prev());
	}

	Eigen::VectorXd ImplicitNewmark::compute_velocity(const Eigen::VectorXd &x) const
	{
		const double c = gamma() / beta();
		return c / dt() * (x - x_prev()) + (1 - c) * v_prev() + (1 - c / 2) * dt() * a_prev();
	}

	Eigen::VectorXd ImplicitNewmark::compute_acceleration(const Eigen::VectorXd &v) const
	{
		return (v - v_prev() - (1 - gamma()) * dt() * a_prev()) / (gamma() * dt());
	}

	double ImplicitNewmark::acceleration_scaling() const
	{
		return beta() * dt() * dt();
	}
} // namespace polyfem::time_integrator
