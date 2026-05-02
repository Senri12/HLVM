import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Array;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.rmi.Remote;

public final class SlBridge {
    private static final String GENERATED_CLASS = "SimpleLangProgram";
    private static final Map<Integer, RemoteObjectRef> REMOTE_OBJECTS = new HashMap<Integer, RemoteObjectRef>();
    private static int nextRemoteObjectId = 0;

    private SlBridge() {
    }

    private static final class RemoteObjectRef implements Remote {
        private final String typeName;
        private final int heapHandle;

        private RemoteObjectRef(String typeName, int heapHandle) {
            this.typeName = typeName;
            this.heapHandle = heapHandle;
        }
    }

    public static void main(String[] args) throws Exception {
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in, "UTF-8"));
        String line;

        System.out.println("ready");
        System.out.flush();

        while ((line = input.readLine()) != null) {
            line = line.trim();
            if (line.length() == 0) {
                continue;
            }
            if ("quit".equals(line)) {
                System.out.println("bye");
                System.out.flush();
                return;
            }

            try {
                System.out.println(handle(line));
            } catch (Exception ex) {
                System.out.println("error " + sanitize(ex.getMessage()));
            }
            System.out.flush();
        }
    }

    private static String handle(String line) throws Exception {
        List<String> parts = splitCommand(line);
        if (parts.size() < 2) {
            return "error expected: call <function> [args...] or new <type> <function> [args...]";
        }

        if ("new".equals(parts.get(0))) {
            if (parts.size() < 3) {
                return "error expected: new <type> <function> [args...]";
            }
            String typeName = parts.get(1);
            String functionName = parts.get(2);
            List<String> argTokens = parts.subList(3, parts.size());
            Object result = invokeGenerated(functionName, argTokens);
            if (!(result instanceof Number)) {
                return "error constructor function did not return a heap handle";
            }
            int heapHandle = ((Number) result).intValue();
            int remoteId = nextRemoteObjectId++;
            REMOTE_OBJECTS.put(Integer.valueOf(remoteId), new RemoteObjectRef(typeName, heapHandle));
            return "ok @" + remoteId;
        }

        if (!"call".equals(parts.get(0))) {
            return "error expected: call <function> [args...] or new <type> <function> [args...]";
        }

        String functionName = parts.get(1);
        List<String> argTokens = parts.subList(2, parts.size());
        Method method = findGeneratedMethod(Class.forName(GENERATED_CLASS), functionName, argTokens);
        Object result = invokeGenerated(method, argTokens);

        if (method.getReturnType() == Void.TYPE) {
            return "ok";
        }
        return "ok " + formatValue(result);
    }

    private static Object invokeGenerated(String functionName, List<String> argTokens) throws Exception {
        Class<?> generatedClass = Class.forName(GENERATED_CLASS);
        Method method = findGeneratedMethod(generatedClass, functionName, argTokens);
        return invokeGenerated(method, argTokens);
    }

    private static Object invokeGenerated(Method method, List<String> argTokens) throws Exception {
        Object[] values = convertArguments(method.getParameterTypes(), argTokens);
        Object result;
        try {
            result = method.invoke(null, values);
        } catch (InvocationTargetException ex) {
            Throwable cause = ex.getCause();
            throw new Exception(cause == null ? ex.toString() : cause.toString());
        }
        return result;
    }

    public static String functions() throws ClassNotFoundException {
        Class<?> generatedClass = Class.forName(GENERATED_CLASS);
        Method[] methods = generatedClass.getMethods();
        List<String> names = new ArrayList<String>();

        for (int i = 0; i < methods.length; i++) {
            Method method = methods[i];
            if (method.getDeclaringClass() != generatedClass) {
                continue;
            }
            if ("main".equals(method.getName()) || method.getName().startsWith("__")) {
                continue;
            }
            names.add(method.getName() + "/" + method.getParameterTypes().length);
        }

        return names.toString();
    }

    private static Method findGeneratedMethod(Class<?> generatedClass, String sourceName, List<String> argTokens)
            throws NoSuchMethodException {
        String prefix = "global$" + sourceName + "$" + argTokens.size() + "$";
        Method[] methods = generatedClass.getMethods();
        NoSuchMethodException lastError = null;

        for (int i = 0; i < methods.length; i++) {
            Method method = methods[i];
            if (method.getDeclaringClass() != generatedClass) {
                continue;
            }
            if (!method.getName().equals(sourceName) && !method.getName().startsWith(prefix)) {
                continue;
            }
            if (method.getParameterTypes().length != argTokens.size()) {
                continue;
            }
            try {
                convertArguments(method.getParameterTypes(), argTokens);
                return method;
            } catch (IllegalArgumentException ex) {
                lastError = new NoSuchMethodException(ex.getMessage());
            }
        }

        if (lastError != null) {
            throw lastError;
        }
        throw new NoSuchMethodException(sourceName + "/" + argTokens.size());
    }

    private static Object[] convertArguments(Class<?>[] parameterTypes, List<String> tokens) {
        Object[] values = new Object[parameterTypes.length];
        for (int i = 0; i < parameterTypes.length; i++) {
            values[i] = convertValue(parameterTypes[i], tokens.get(i));
        }
        return values;
    }

    private static Object convertValue(Class<?> type, String token) {
        if (token.startsWith("@")) {
            RemoteObjectRef ref = resolveRemoteObject(token);
            if (type == Integer.TYPE || type == Integer.class) {
                return Integer.valueOf(ref.heapHandle);
            }
            throw new IllegalArgumentException("remote object " + token + " cannot be passed as " + type.getName());
        }
        if (type == String.class) {
            return parseString(token);
        }
        if (type == Character.TYPE || type == Character.class) {
            String text = parseString(token);
            if (text.length() != 1) {
                throw new IllegalArgumentException("expected one character, got: " + token);
            }
            return Character.valueOf(text.charAt(0));
        }
        if (type == Boolean.TYPE || type == Boolean.class) {
            if ("true".equalsIgnoreCase(token) || "1".equals(token)) {
                return Boolean.TRUE;
            }
            if ("false".equalsIgnoreCase(token) || "0".equals(token)) {
                return Boolean.FALSE;
            }
            throw new IllegalArgumentException("expected boolean, got: " + token);
        }
        if (type == Byte.TYPE || type == Byte.class) {
            return Byte.valueOf(token);
        }
        if (type == Short.TYPE || type == Short.class) {
            return Short.valueOf(token);
        }
        if (type == Integer.TYPE || type == Integer.class) {
            return Integer.valueOf(token);
        }
        if (type == Long.TYPE || type == Long.class) {
            return Long.valueOf(token);
        }
        if (type == Float.TYPE || type == Float.class) {
            return Float.valueOf(token);
        }
        if (type == Double.TYPE || type == Double.class) {
            return Double.valueOf(token);
        }
        if (type.isArray()) {
            return parseArray(type.getComponentType(), token);
        }
        throw new IllegalArgumentException("unsupported JVM parameter type: " + type.getName());
    }

    private static RemoteObjectRef resolveRemoteObject(String token) {
        int remoteId;
        try {
            remoteId = Integer.parseInt(token.substring(1));
        } catch (NumberFormatException ex) {
            throw new IllegalArgumentException("invalid remote object reference: " + token);
        }
        RemoteObjectRef ref = REMOTE_OBJECTS.get(Integer.valueOf(remoteId));
        if (ref == null) {
            throw new IllegalArgumentException("unknown remote object reference: " + token);
        }
        return ref;
    }

    private static Object parseArray(Class<?> componentType, String token) {
        if (!token.startsWith("[") || !token.endsWith("]")) {
            throw new IllegalArgumentException("expected array literal, got: " + token);
        }
        List<String> items = splitArrayItems(token.substring(1, token.length() - 1));
        Object array = Array.newInstance(componentType, items.size());
        for (int i = 0; i < items.size(); i++) {
            Array.set(array, i, convertValue(componentType, items.get(i)));
        }
        return array;
    }

    private static String formatValue(Object value) {
        if (value == null) {
            return "null";
        }
        Class<?> type = value.getClass();
        if (type == String.class || type == Character.class) {
            return quote(String.valueOf(value));
        }
        if (type.isArray()) {
            int length = Array.getLength(value);
            StringBuilder out = new StringBuilder();
            out.append('[');
            for (int i = 0; i < length; i++) {
                if (i > 0) {
                    out.append(',');
                }
                out.append(formatValue(Array.get(value, i)));
            }
            out.append(']');
            return out.toString();
        }
        return String.valueOf(value);
    }

    private static String parseString(String token) {
        if (token.length() >= 2 && token.charAt(0) == '"' && token.charAt(token.length() - 1) == '"') {
            StringBuilder out = new StringBuilder();
            for (int i = 1; i < token.length() - 1; i++) {
                char c = token.charAt(i);
                if (c == '\\' && i + 1 < token.length() - 1) {
                    char next = token.charAt(++i);
                    if (next == 'n') out.append('\n');
                    else if (next == 'r') out.append('\r');
                    else if (next == 't') out.append('\t');
                    else out.append(next);
                } else {
                    out.append(c);
                }
            }
            return out.toString();
        }
        return token;
    }

    private static String quote(String text) {
        StringBuilder out = new StringBuilder();
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '\\' || c == '"') out.append('\\').append(c);
            else if (c == '\n') out.append("\\n");
            else if (c == '\r') out.append("\\r");
            else if (c == '\t') out.append("\\t");
            else out.append(c);
        }
        out.append('"');
        return out.toString();
    }

    private static List<String> splitCommand(String line) {
        return splitDelimited(line, ' ', false);
    }

    private static List<String> splitArrayItems(String text) {
        return splitDelimited(text, ',', true);
    }

    private static List<String> splitDelimited(String text, char delimiter, boolean keepEmpty) {
        List<String> parts = new ArrayList<String>();
        StringBuilder current = new StringBuilder();
        boolean inString = false;
        boolean escaped = false;
        int depth = 0;

        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (escaped) {
                current.append(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                current.append(c);
                escaped = true;
                continue;
            }
            if (c == '"') {
                current.append(c);
                inString = !inString;
                continue;
            }
            if (!inString) {
                if (c == '[') depth++;
                if (c == ']') depth--;
                if (depth == 0 && (c == delimiter || (delimiter == ' ' && Character.isWhitespace(c)))) {
                    String item = current.toString().trim();
                    if (keepEmpty || item.length() > 0) {
                        parts.add(item);
                    }
                    current.setLength(0);
                    continue;
                }
            }
            current.append(c);
        }
        String item = current.toString().trim();
        if (keepEmpty || item.length() > 0) {
            parts.add(item);
        }
        return parts;
    }

    private static String sanitize(String message) {
        if (message == null || message.length() == 0) {
            return "unknown";
        }
        return message.replace('\r', ' ').replace('\n', ' ');
    }
}
