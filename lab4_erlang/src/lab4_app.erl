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
                print_call(State, sumRange, [1, 10]),
                print_call(State, notBool, [true]),
                print_call(State, nextChar, [{char, $A}]),
                print_call(State, incByte, [41]),
                print_call(State, echoLong, [1234567890123]),
                print_call(State, echoString, [{string, "hello"}]),
                print_call(State, acceptMany,
                           [true, {char, $Z}, 7, 1234567890123, {string, "ok"}]),
                demo_user_type(State)
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
            io:format("~s(~s) = ~p~n", [atom_to_list(Function), join_args(Args), Value]);
        ok ->
            io:format("~s(~s) = ok~n", [atom_to_list(Function), join_args(Args)]);
        {error, Reason} ->
            io:format("~s(~s) failed: ~p~n", [atom_to_list(Function), join_args(Args), Reason])
    end.

join_args([]) ->
    "";
join_args([Only]) ->
    format_demo_arg(Only);
join_args([Head | Tail]) ->
    format_demo_arg(Head) ++ ", " ++ join_args(Tail).

format_demo_arg({char, Value}) when is_integer(Value) ->
    "'" ++ [Value] ++ "'";
format_demo_arg({string, Text}) ->
    "\"" ++ Text ++ "\"";
format_demo_arg({object, TypeName, Handle}) ->
    atom_to_list(TypeName) ++ "#" ++ integer_to_list(Handle);
format_demo_arg(Value) when is_integer(Value) ->
    integer_to_list(Value);
format_demo_arg(Value) when is_float(Value) ->
    float_to_list(Value);
format_demo_arg(true) ->
    "true";
format_demo_arg(false) ->
    "false";
format_demo_arg(Value) when is_atom(Value) ->
    atom_to_list(Value).

demo_user_type(State) ->
    case sl_port:call_object(State, 'Vec2i', {makeVec2i, [12, 30]}) of
        {ok, Vec} ->
            io:format("makeVec2i(12, 30) = ~s~n", [format_demo_arg(Vec)]),
            print_call(State, vecX, [Vec]),
            print_call(State, vecY, [Vec]),
            print_call(State, vecSum, [Vec]);
        Other ->
            io:format("makeVec2i(12, 30) failed: ~p~n", [Other])
    end.
