-module(sl_port).

-export([start/0, start/1, stop/1, call/2]).

-record(state, {port}).

start() ->
    case os:getenv("LAB4_CLASSPATH") of
        false ->
            start("build/lab4_app");
        ClassPath ->
            start(ClassPath)
    end.

start(ClassPath) ->
    Command = "java -cp " ++ quote(ClassPath) ++ " SlBridge",
    Port = open_port({spawn, Command}, [{line, 4096}, exit_status, use_stdio, stderr_to_stdout]),
    case recv_line(Port, 5000) of
        {ok, "ready"} ->
            {ok, #state{port = Port}};
        {ok, Other} ->
            stop(#state{port = Port}),
            {error, {unexpected_startup_response, Other}};
        Error ->
            stop(#state{port = Port}),
            Error
    end.

stop(#state{port = Port}) ->
    try port_command(Port, "quit\n") of
        true ->
            receive
                {Port, {data, {eol, "bye"}}} ->
                    ok;
                {Port, {exit_status, _Status}} ->
                    ok
            after 1000 ->
                catch port_close(Port),
                ok
            end
    catch
        error:badarg ->
            ok
    end.

call(#state{port = Port}, {Function, Args}) when is_atom(Function), is_list(Args) ->
    call(#state{port = Port}, {atom_to_list(Function), Args});
call(#state{port = Port}, {Function, Args}) when is_list(Function), is_list(Args) ->
    Line = "call " ++ Function ++ format_args(Args) ++ "\n",
    port_command(Port, Line),
    case recv_line(Port, 5000) of
        {ok, "ok"} ->
            ok;
        {ok, "ok " ++ ValueText} ->
            {ok, list_to_integer(ValueText)};
        {ok, "error " ++ Reason} ->
            {error, Reason};
        {ok, Other} ->
            {error, {unexpected_response, Other}};
        Error ->
            Error
    end.

format_args([]) ->
    "";
format_args([Head | Tail]) when is_integer(Head) ->
    " " ++ integer_to_list(Head) ++ format_args(Tail).

recv_line(Port, Timeout) ->
    receive
        {Port, {data, {eol, Line}}} ->
            {ok, Line};
        {Port, {data, {noeol, Line}}} ->
            {ok, Line};
        {Port, {exit_status, Status}} ->
            {error, {port_exit, Status}}
    after Timeout ->
        {error, timeout}
    end.

quote(Text) ->
    "\"" ++ escape_quotes(Text) ++ "\"".

escape_quotes([]) ->
    [];
escape_quotes([$" | Tail]) ->
    [$\\, $" | escape_quotes(Tail)];
escape_quotes([Head | Tail]) ->
    [Head | escape_quotes(Tail)].
