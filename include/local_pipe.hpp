/* local_pipe: basically a list that can be async waited on
 * these pipes work completely inside the process so no real pipe
 * fds are used
 * you can use any type, you dont need to serialize anything to a
 * buffer or try passing around loose pointers
 * can be good for thread based actor models
 * not thread safe so if you do use it for actor models make sure you
 * wrap it with mutexes
*/

#pragma once

#include <list>
#include <boost/asio/executor.hpp>
#include <boost/asio/async_result.hpp>

// override if you want to
#ifndef CTIL_BIND_FRONT
#include <boost/beast/core/bind_handler.hpp>
#define CTIL_BIND_FRONT boost::beast::bind_front_handler
#endif

namespace ctil {

template <class T, class Executor = boost::asio::executor>
struct local_pipe {
	using element_type = T;
	using executor_type = Executor;

	template <class... ExecutorArg>
	local_pipe(ExecutorArg&&... a):/**/
		exec(std::forward<ExecutorArg>(a)...)
	/**/{

	}

	/* error codes:
	 *	boost::system::errc::device_or_resource_busy: theres already
		a handler waiting
	 *	boost::asio::error::operation_aborted: cancel() called or
		pipe destructed while you were waiting. sorry about that.

	 *	if an element is available, posts the supplied token immediately
	 *	else stores the token until an element is available
	*/

	template <class CompletionToken>
	using async_wait_result = boost::asio::async_result<
		std::decay_t<CompletionToken>,
		void(boost::system::error_code)
	>;

	template <class Token>
	typename async_wait_result<Token>::return_type async_wait(Token &&tok){
		using result_type = async_wait_result<Token>;
		typename result_type::completion_handler_type hand(std::forward<Token>(tok));
		result_type result(hand);

		if(waiting){
			boost::asio::post(
				exec,
				CTIL_BIND_FRONT(
					std::move(hand),
					boost::system::error_code(boost::system::errc::device_or_resource_busy, boost::system::generic_category())
				)
			);
			return result.get();
		}
		if(size() != 0){
			boost::asio::post(
				exec,
				CTIL_BIND_FRONT(
					std::move(hand),
					boost::system::error_code()
				)
			);
		} else waiting = std::make_unique<wait_op<Token>>(hand);
		return result.get();
	}

	//same thing, same errors, but also does the read for you

	template <class CompletionToken>
	using async_read_result = boost::asio::async_result<
		std::decay_t<CompletionToken>,
		void(boost::system::error_code, element_type)
	>;

	template <class Token>
	typename async_read_result<Token>::return_type async_read(Token &&tok){
		using result_type = async_read_result<Token>;
		typename result_type::completion_handler_type hand(std::forward<Token>(tok));
		result_type result(hand);

		async_wait([&, hand = std::move(hand)](boost::system::error_code ec){
			hand(ec, ec ? element_type() : read());
		});
		return result.get();
	}

	//enqueues an element and wakes up the currently waiting handler
	template <class... Args>
	void emplace(Args&&... a){
		in_flight.emplace_back(std::forward<Args>(a)...);
		if(waiting) std::exchange(waiting, {})->go();
	}

	executor_type &get_executor() noexcept {
		return exec;
	}

	void cancel(){
		if(waiting) std::exchange(waiting, {})->cancel();
	}

	size_t size() const noexcept {
		return in_flight.size();
	}

	//returns the next element in flight or throws on error
	element_type read(){
		boost::system::error_code ec;
		element_type ret = read(ec);
		if(ec) throw boost::system::system_error(ec);
		return ret;
	}

	/* reads the next element in flight
	 * errors:
	 *	boost::system::errc::device_or_resource_busy: handler
		already waiting (you cant read while waiting)
	 *	boost::system::errc::operation_would_block: no elements
		are available to read (you can check with size())
	*/
	element_type read(boost::system::error_code &ec){
		if(waiting){
			ec.assign(boost::system::errc::device_or_resource_busy, boost::system::generic_category());
			return element_type();
		}
		if(in_flight.empty()){
			ec.assign(boost::system::errc::operation_would_block, boost::system::generic_category());
			return element_type();
		}
		element_type ret = std::move_if_noexcept(in_flight.front());
		in_flight.pop_front();
		return ret;
	}

private:
	struct wait_wrapper {
		virtual ~wait_wrapper(){}
		virtual void go(boost::system::error_code = {}) = 0;
		void cancel(){
			go(boost::asio::error::operation_aborted);
		}
	};

	template <class Token>
	struct wait_op : public wait_wrapper {
		wait_op(Token &_tok):/**/
			tok(std::move(_tok))
		/**/{

		}

		void go(boost::system::error_code ec) final {
			tok(ec);
		}

		std::decay_t<Token> tok;
	};

	executor_type exec;

	std::unique_ptr<wait_wrapper> waiting;
	std::list<element_type> in_flight;
};

} //end namespace ctil


//i might make some pipes based on a circular buffer later if you want
