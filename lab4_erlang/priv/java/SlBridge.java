import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

public final class SlBridge {
    private static final String GENERATED_CLASS = "SimpleLangProgram";

    private SlBridge() {
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
        String[] parts = line.split("\\s+");
        if (parts.length < 2 || !"call".equals(parts[0])) {
            return "error expected: call <function> [int...]";
        }

        String functionName = parts[1];
        int arity = parts.length - 2;
        Class<?>[] signature = new Class<?>[arity];
        Object[] values = new Object[arity];

        for (int i = 0; i < arity; i++) {
            signature[i] = Integer.TYPE;
            values[i] = Integer.valueOf(parts[i + 2]);
        }

        Class<?> generatedClass = Class.forName(GENERATED_CLASS);
        Method method = findGeneratedMethod(generatedClass, functionName, signature);
        Object result;
        try {
            result = method.invoke(null, values);
        } catch (InvocationTargetException ex) {
            Throwable cause = ex.getCause();
            throw new Exception(cause == null ? ex.toString() : cause.toString());
        }

        if (method.getReturnType() == Void.TYPE) {
            return "ok";
        }
        return "ok " + result;
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

    private static Method findGeneratedMethod(Class<?> generatedClass, String sourceName, Class<?>[] signature)
            throws NoSuchMethodException {
        try {
            return generatedClass.getMethod(sourceName, signature);
        } catch (NoSuchMethodException ignored) {
            String prefix = "global$" + sourceName + "$" + signature.length + "$";
            Method[] methods = generatedClass.getMethods();
            for (int i = 0; i < methods.length; i++) {
                Method method = methods[i];
                if (method.getDeclaringClass() == generatedClass
                        && method.getName().startsWith(prefix)
                        && sameSignature(method.getParameterTypes(), signature)) {
                    return method;
                }
            }
            throw ignored;
        }
    }

    private static boolean sameSignature(Class<?>[] left, Class<?>[] right) {
        if (left.length != right.length) {
            return false;
        }
        for (int i = 0; i < left.length; i++) {
            if (left[i] != right[i]) {
                return false;
            }
        }
        return true;
    }

    private static String sanitize(String message) {
        if (message == null || message.length() == 0) {
            return "unknown";
        }
        return message.replace('\r', ' ').replace('\n', ' ');
    }
}
