#include <eosio.token/eosio.token.hpp>
#include <eosio/transaction.hpp>

namespace eosio
{
#pragma region basic

	void token::create(const name &issuer,
					   const asset &maximum_supply)
	{
		require_auth(get_self());

		auto sym = maximum_supply.symbol;
		check(sym.is_valid(), "invalid symbol name");
		check(maximum_supply.is_valid(), "invalid supply");
		check(maximum_supply.amount > 0, "max-supply must be positive");

		stats statstable(get_self(), sym.code().raw());
		auto existing = statstable.find(sym.code().raw());
		check(existing == statstable.end(), "token with symbol already exists");

		statstable.emplace(get_self(), [&](auto &s) {
			s.supply.symbol = maximum_supply.symbol;
			s.max_supply = maximum_supply;
			s.issuer = issuer;
		});
	}

	void token::issue(const name &to, const asset &quantity, const string &memo)
	{
		auto sym = quantity.symbol;
		check(sym.is_valid(), "invalid symbol name");
		check(memo.size() <= 256, "memo has more than 256 bytes");

		stats statstable(get_self(), sym.code().raw());
		auto existing = statstable.find(sym.code().raw());
		check(existing != statstable.end(), "token with symbol does not exist, create token before issue");
		const auto &st = *existing;
		//check(to == st.issuer, "tokens can only be issued to issuer account");

		require_auth(st.issuer);
		check(quantity.is_valid(), "invalid quantity");
		check(quantity.amount > 0, "must issue positive quantity");

		check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
		check(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

		statstable.modify(st, same_payer, [&](auto &s) {
			s.supply += quantity;
		});

		add_balance(to, quantity, st.issuer);
	}

	void token::retire(const asset &quantity, const string &memo)
	{
		auto sym = quantity.symbol;
		check(sym.is_valid(), "invalid symbol name");
		check(memo.size() <= 256, "memo has more than 256 bytes");

		stats statstable(get_self(), sym.code().raw());
		auto existing = statstable.find(sym.code().raw());
		check(existing != statstable.end(), "token with symbol does not exist");
		const auto &st = *existing;

		require_auth(st.issuer);
		check(quantity.is_valid(), "invalid quantity");
		check(quantity.amount > 0, "must retire positive quantity");

		check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

		statstable.modify(st, same_payer, [&](auto &s) {
			s.supply -= quantity;
		});

		sub_balance(st.issuer, quantity);
	}

	void token::transfer(const name &from,
						 const name &to,
						 const asset &quantity,
						 const string &memo)
	{
		check(from != to, "cannot transfer to self");
		require_auth(from);
		check(is_account(to), "to account does not exist");
		auto sym = quantity.symbol.code();
		stats statstable(get_self(), sym.raw());
		const auto &st = statstable.get(sym.raw());

		require_recipient(from);
		require_recipient(to);

		check(quantity.is_valid(), "invalid quantity");
		check(quantity.amount > 0, "must transfer positive quantity");
		check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
		check(memo.size() <= 256, "memo has more than 256 bytes");

		auto payer = has_auth(to) ? to : from;

		sub_balance(from, quantity);
		add_balance(to, quantity, payer);
	}

	void token::sub_balance(const name &owner, const asset &value)
	{
		accounts from_acnts(get_self(), owner.value);
		locks from_locks(get_self(), owner.value);

		const auto &from = from_acnts.get(value.symbol.code().raw(), "no balance object found");
		const auto from_lock_itr = from_locks.find(value.symbol.code().raw());
		const auto from_lock = from_lock_itr == from_locks.end() ? asset(0, value.symbol) : from_lock_itr->locked;

		check(from.balance.amount - from_lock.amount >= value.amount, "overdrawn balance");

		from_acnts.modify(from, owner, [&](auto &a) {
			a.balance -= value;
		});
	}

	void token::add_balance(const name &owner, const asset &value, const name &ram_payer)
	{
		accounts to_acnts(get_self(), owner.value);
		auto to = to_acnts.find(value.symbol.code().raw());
		if (to == to_acnts.end())
		{
			to_acnts.emplace(ram_payer, [&](auto &a) {
				a.balance = value;
			});
		}
		else
		{
			to_acnts.modify(to, same_payer, [&](auto &a) {
				a.balance += value;
			});
		}
	}

	void token::open(const name &owner, const symbol &symbol, const name &ram_payer)
	{
		require_auth(ram_payer);

		check(is_account(owner), "owner account does not exist");

		auto sym_code_raw = symbol.code().raw();
		stats statstable(get_self(), sym_code_raw);
		const auto &st = statstable.get(sym_code_raw, "symbol does not exist");
		check(st.supply.symbol == symbol, "symbol precision mismatch");

		accounts acnts(get_self(), owner.value);
		auto it = acnts.find(sym_code_raw);
		if (it == acnts.end())
		{
			acnts.emplace(ram_payer, [&](auto &a) {
				a.balance = asset{0, symbol};
			});
		}
	}

	void token::close(const name &owner, const symbol &symbol)
	{
		require_auth(owner);
		accounts acnts(get_self(), owner.value);
		auto it = acnts.find(symbol.code().raw());
		check(it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect.");
		check(it->balance.amount == 0, "Cannot close because the balance is not zero.");
		acnts.erase(it);
	}

#pragma endregion

#pragma region lock

	void token::lock(const name &owner, const asset &quantity)
	{
		require_auth(get_self());

		locks lockstable(get_self(), owner.value);
		auto lock_itr = lockstable.find(quantity.symbol.code().raw());
		if (lock_itr == lockstable.end())
		{
			lockstable.emplace(get_self(), [&](auto &a) {
				a.locked = quantity;
			});
		}
		else
		{
			lockstable.modify(lock_itr, same_payer, [&](auto &a) {
				a.locked = quantity;
			});
		}
	}

	void token::dailyrelease(const name &owner, const asset &quantity)
	{
		require_auth(get_self());

		locks lockstable(get_self(), owner.value);
		auto lock_itr = lockstable.find(quantity.symbol.code().raw());
		check(lock_itr != lockstable.end(), "please lock some token first.");

		if (lock_itr->release_status == 0)
		{
			return;
		}

		auto release_quantity = lock_itr->locked < quantity ? lock_itr->locked : quantity;
		auto released_days = lock_itr->locked.amount <= 0 ? lock_itr->released_days : (lock_itr->released_days + 1);

		lockstable.modify(lock_itr, same_payer, [&](auto &a) {
			a.locked -= release_quantity;
			a.release_status = a.locked.amount > 0 ? 1 : 0;
			a.released_days = released_days;
		});

		if (lock_itr->locked.amount > 0)
		{
			deferred_release(owner, lock_itr->release_per_day, lock_itr->start_release_date, released_days);
		}
	}

	void token::release(const name &owner, const asset &quantity)
	{
		require_auth(get_self());

		locks lockstable(get_self(), owner.value);
		auto lock_itr = lockstable.find(quantity.symbol.code().raw());
		check(lock_itr != lockstable.end(), "please lock some token first.");
		check(lock_itr->locked >= quantity, "overdrawn locked balance.");

		lockstable.modify(lock_itr, same_payer, [&](auto &a) {
			a.locked -= quantity;
		});
	}

	void token::setrelease(const name &owner, const asset &release_per_day, const uint64_t start_date,
						   const uint32_t released_days)
	{
		require_auth(get_self());

		locks lockstable(get_self(), owner.value);
		auto lock_itr = lockstable.find(release_per_day.symbol.code().raw());
		check(lock_itr != lockstable.end(), "please lock some token first.");

		lockstable.modify(lock_itr, same_payer, [&](auto &a) {
			a.release_status = 1;
			a.release_per_day = release_per_day;
			a.start_release_date = start_date;
			a.released_days = released_days;
		});

		deferred_release(owner, release_per_day, start_date, released_days);
	}

	void token::deferred_release(const name &owner, const asset &release_quantity,
								 const uint64_t &start_date, const uint32_t &released_days)
	{
		auto now = current_time_point().elapsed.count() / 1000;
		auto next_release_time = start_date + (released_days + 1) * milliseconds_per_day;
		auto delay_sec = next_release_time < now ? 10 : (next_release_time - now) / 1000;

		eosio::transaction out;
		out.actions.emplace_back(permission_level{get_self(), active_permission},
								 get_self(), "dailyrelease"_n,
								 std::make_tuple(owner, release_quantity));
		out.delay_sec = delay_sec;
		eosio::cancel_deferred(owner.value);
		out.send(owner.value, get_self(), true);
	}

#pragma endregion

#pragma region dailyissue

	void token::dailyissue(const uint64_t &id, const name &receiver, const asset &quantity)
	{
		require_auth(get_self());

		dailyissues dlyisstable(get_self(), get_self().value);
		auto dlyiss_itr = dlyisstable.find(id);
		check(dlyiss_itr != dlyisstable.end(), "please set dailyissue first.");

		if (dlyiss_itr->status == 0)
		{
			return;
		}

		check(dlyiss_itr->issue_per_day == quantity, "setting changed error.");
		check(dlyiss_itr->receiver == receiver, "setting changed error.");

		string memo = "dailyissue id:" + std::to_string(id);
		token::issue_action issue_act{get_self(), {{token::system_account, token::active_permission}}};
		issue_act.send(receiver, quantity, memo);

		// token::transfer_action transfer_act{get_self(), {{get_self(), token::active_permission}}};
		// transfer_act.send(get_self(), receiver, quantity, memo);

		auto issued_days = dlyiss_itr->issued_days + 1;
		dlyisstable.modify(dlyiss_itr, same_payer, [&](auto &a) {
			a.issued_days = issued_days;
		});

		deferred_issue(id, receiver, quantity, dlyiss_itr->start_issue_date, issued_days);
	}

	void token::switchdlyiss(const uint64_t &id, const uint8_t &status)
	{
		require_auth(get_self());

		dailyissues dlyisstable(get_self(), get_self().value);
		auto dlyiss_itr = dlyisstable.find(id);
		check(dlyiss_itr != dlyisstable.end(), "please set dailyissue first.");

		dlyisstable.modify(dlyiss_itr, same_payer, [&](auto &a) {
			a.status = status;
		});

		if (status == 0)
		{
			eosio::cancel_deferred(id);
		}
		else
		{
			deferred_issue(id, dlyiss_itr->receiver, dlyiss_itr->issue_per_day, dlyiss_itr->start_issue_date, dlyiss_itr->issued_days);
		}
	}

	void token::setdlyiss(const uint64_t &id, const name &receiver, const asset &issue_per_day, const uint64_t start_date, const uint32_t issued_days)
	{
		require_auth(get_self());

		dailyissues dlyisstable(get_self(), get_self().value);
		auto dlyiss_itr = dlyisstable.find(id);
		if (dlyiss_itr == dlyisstable.end())
		{
			dlyisstable.emplace(get_self(), [&](auto &a) {
				a.id = id;
				a.receiver = receiver;
				a.status = 0;
				a.issue_per_day = issue_per_day;
				a.start_issue_date = start_date;
				a.issued_days = issued_days;
			});
		}
		else
		{
			dlyisstable.modify(dlyiss_itr, same_payer, [&](auto &a) {
				a.receiver = receiver;
				a.issue_per_day = issue_per_day;
				a.start_issue_date = start_date;
				a.issued_days = issued_days;
			});
		}
	}

	void token::deferred_issue(const uint64_t &id, const name &receiver, const asset &quantity,
							   const uint64_t &start_date, const uint32_t &issued_days)
	{
		auto now = current_time_point().elapsed.count() / 1000;
		auto next_issue_time = start_date + (issued_days + 1) * milliseconds_per_day;
		auto delay_sec = next_issue_time < now ? 10 : (next_issue_time - now) / 1000;

		eosio::transaction out;
		out.actions.emplace_back(permission_level{get_self(), active_permission},
								 get_self(), "dailyissue"_n,
								 std::make_tuple(id, receiver, quantity));
		out.delay_sec = delay_sec;
		eosio::cancel_deferred(id);
		out.send(id, get_self(), true);
	}

#pragma endregion

} // namespace eosio
