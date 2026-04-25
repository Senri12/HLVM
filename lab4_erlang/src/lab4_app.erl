-module(lab4_app).

-export([main/0, demo/0]).

main() ->
    case sl_port:start() of
        {ok, State} ->
            try
                loop(State)
            after
                sl_port:stop(State)
            end;
        {error, Reason} ->
            io:format("Cannot start SimpleLang JVM bridge: ~p~n", [Reason])
    end.

demo() ->
    case sl_port:start() of
        {ok, State} ->
            try
                print_call(State, gcd, [48, 18]),
                print_call(State, lcm, [21, 6]),
                print_call(State, isPrime, [97]),
                print_call(State, fib, [10]),
                print_call(State, sumRange, [1, 10])
            after
                sl_port:stop(State)
            end;
        {error, Reason} ->
            io:format("Cannot start SimpleLang JVM bridge: ~p~n", [Reason])
    end.

loop(State) ->
    print_menu(),
    case read_int("Select operation") of
        0 ->
            ok;
        1 ->
            A = read_int("a"),
            B = read_int("b"),
            print_call(State, gcd, [A, B]),
            loop(State);
        2 ->
            A = read_int("a"),
            B = read_int("b"),
            print_call(State, lcm, [A, B]),
            loop(State);
        3 ->
            N = read_int("n"),
            print_call(State, isPrime, [N]),
            loop(State);
        4 ->
            N = read_int("n"),
            print_call(State, fib, [N]),
            loop(State);
        5 ->
            A = read_int("from"),
            B = read_int("to"),
            print_call(State, sumRange, [A, B]),
            loop(State);
        _ ->
            io:format("Unknown menu item~n"),
            loop(State)
    end.

print_menu() ->
    io:format("~nSimpleLang/JVM functions from Erlang~n"),
    io:format("1. gcd(a, b)~n"),
    io:format("2. lcm(a, b)~n"),
    io:format("3. isPrime(n)~n"),
    io:format("4. fib(n)~n"),
    io:format("5. sumRange(a, b)~n"),
    io:format("0. exit~n").

read_int(Prompt) ->
    Line = io:get_line(Prompt ++ ": "),
    case string:to_integer(string:strip(Line)) of
        {Value, _Rest} when is_integer(Value) ->
            Value;
        {error, _Reason} ->
            io:format("Please enter an integer.~n"),
            read_int(Prompt)
    end.

print_call(State, Function, Args) ->
    case sl_port:call(State, {Function, Args}) of
        {ok, Value} ->
            io:format("~s(~s) = ~p~n", [atom_to_list(Function), join_ints(Args), Value]);
        ok ->
            io:format("~s(~s) = ok~n", [atom_to_list(Function), join_ints(Args)]);
        {error, Reason} ->
            io:format("~s(~s) failed: ~p~n", [atom_to_list(Function), join_ints(Args), Reason])
    end.

join_ints([]) ->
    "";
join_ints([Only]) ->
    integer_to_list(Only);
join_ints([Head | Tail]) ->
    integer_to_list(Head) ++ ", " ++ join_ints(Tail).
