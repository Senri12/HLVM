-module(sl_port).

-export([start/0, start/1, stop/1, call/2, call_object/3]).

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
            {ok, parse_value(ValueText)};
        {ok, "error " ++ Reason} ->
            {error, Reason};
        {ok, Other} ->
            {error, {unexpected_response, Other}};
        Error ->
            Error
    end.

call_object(#state{port = Port}, TypeName, {Function, Args}) when is_atom(TypeName), is_atom(Function), is_list(Args) ->
    call_object(#state{port = Port}, atom_to_list(TypeName), {atom_to_list(Function), Args});
call_object(#state{port = Port}, TypeName, {Function, Args}) when is_atom(TypeName), is_list(Function), is_list(Args) ->
    call_object(#state{port = Port}, atom_to_list(TypeName), {Function, Args});
call_object(#state{port = Port}, TypeName, {Function, Args}) when is_list(TypeName), is_atom(Function), is_list(Args) ->
    call_object(#state{port = Port}, TypeName, {atom_to_list(Function), Args});
call_object(#state{port = Port}, TypeName, {Function, Args}) when is_list(TypeName), is_list(Function), is_list(Args) ->
    Line = "new " ++ TypeName ++ " " ++ Function ++ format_args(Args) ++ "\n",
    port_command(Port, Line),
    case recv_line(Port, 5000) of
        {ok, "ok @" ++ RefText} ->
            case string:to_integer(RefText) of
                {RemoteId, []} when is_integer(RemoteId) ->
                    {ok, {object, list_to_atom(TypeName), RemoteId}};
                _ ->
                    {error, {bad_remote_reference, RefText}}
            end;
        {ok, "error " ++ Reason} ->
            {error, Reason};
        {ok, Other} ->
            {error, {unexpected_response, Other}};
        Error ->
            Error
    end.

format_args([]) ->
    "";
format_args([Head | Tail]) ->
    " " ++ format_arg(Head) ++ format_args(Tail).

format_arg(Value) when is_integer(Value) ->
    integer_to_list(Value);
format_arg(Value) when is_float(Value) ->
    float_to_list(Value);
format_arg(true) ->
    "true";
format_arg(false) ->
    "false";
format_arg({char, Value}) when is_integer(Value) ->
    quote([Value]);
format_arg({char, [Value]}) when is_integer(Value) ->
    quote([Value]);
format_arg({string, Text}) ->
    quote(Text);
format_arg({array, Items}) when is_list(Items) ->
    "[" ++ join_encoded(Items) ++ "]";
format_arg({object, _TypeName, RemoteId}) when is_integer(RemoteId) ->
    "@" ++ integer_to_list(RemoteId);
format_arg(Value) when is_atom(Value) ->
    atom_to_list(Value).

join_encoded([]) ->
    "";
join_encoded([Only]) ->
    format_arg(Only);
join_encoded([Head | Tail]) ->
    format_arg(Head) ++ "," ++ join_encoded(Tail).

parse_value("true") ->
    true;
parse_value("false") ->
    false;
parse_value("null") ->
    null;
parse_value([$" | _] = Text) ->
    unquote(Text);
parse_value([$[ | _] = Text) ->
    Text;
parse_value(Text) ->
    case string:to_integer(Text) of
        {Value, []} when is_integer(Value) ->
            Value;
        _ ->
            case string:to_float(Text) of
                {Value, []} when is_float(Value) ->
                    Value;
                _ ->
                    Text
            end
    end.

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
    "\"" ++ escape_text(Text) ++ "\"".

escape_text([]) ->
    [];
escape_text([$" | Tail]) ->
    [$\\, $" | escape_text(Tail)];
escape_text([$\\ | Tail]) ->
    [$\\, $\\ | escape_text(Tail)];
escape_text([$\n | Tail]) ->
    [$\\, $n | escape_text(Tail)];
escape_text([$\r | Tail]) ->
    [$\\, $r | escape_text(Tail)];
escape_text([$\t | Tail]) ->
    [$\\, $t | escape_text(Tail)];
escape_text([Head | Tail]) ->
    [Head | escape_text(Tail)].

unquote(Text) ->
    unquote_inner(tl(lists:sublist(Text, length(Text) - 1))).

unquote_inner([]) ->
    [];
unquote_inner([$\\, $n | Tail]) ->
    [$\n | unquote_inner(Tail)];
unquote_inner([$\\, $r | Tail]) ->
    [$\r | unquote_inner(Tail)];
unquote_inner([$\\, $t | Tail]) ->
    [$\t | unquote_inner(Tail)];
unquote_inner([$\\, Head | Tail]) ->
    [Head | unquote_inner(Tail)];
unquote_inner([Head | Tail]) ->
    [Head | unquote_inner(Tail)].
