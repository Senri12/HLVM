class Vec2i {
    int x;
    int y;

    void init(int a, int b) {
        this.x = a;
        this.y = b;
    }

    int sum() {
        return this.x + this.y;
    }
}

int absInt(int x) {
    if (x < 0) {
        return 0 - x;
    }
    return x;
}

int gcd(int a, int b) {
    int x = absInt(a);
    int y = absInt(b);
    int t = 0;

    while (y != 0) {
        t = x % y;
        x = y;
        y = t;
    }

    return x;
}

int lcm(int a, int b) {
    int g = gcd(a, b);
    int q = 0;
    int result = 0;

    if (g == 0) {
        return 0;
    }

    q = a / g;
    result = q * b;
    return absInt(result);
}

int isPrime(int n) {
    int d = 2;

    if (n < 2) {
        return 0;
    }

    while (d * d <= n) {
        if (n % d == 0) {
            return 0;
        }
        d = d + 1;
    }

    return 1;
}

int fib(int n) {
    int i = 0;
    int a = 0;
    int b = 1;
    int t = 0;

    if (n < 0) {
        return 0;
    }

    while (i < n) {
        t = a + b;
        a = b;
        b = t;
        i = i + 1;
    }

    return a;
}

int sumRange(int a, int b) {
    int i = a;
    int s = 0;

    while (i <= b) {
        s = s + i;
        i = i + 1;
    }

    return s;
}

bool notBool(bool value) {
    return !value;
}

char nextChar(char value) {
    return value + 1;
}

byte incByte(byte value) {
    return value + 1;
}

long echoLong(long value) {
    return value;
}

string echoString(string value) {
    return value;
}

void acceptMany(bool flag, char letter, byte small, long big, string text) {
    return;
}

Vec2i makeVec2i(int x, int y) {
    Vec2i v;
    v.init(x, y);
    return v;
}

int vecX(Vec2i v) {
    return v.x;
}

int vecY(Vec2i v) {
    return v.y;
}

int vecSum(Vec2i v) {
    return v.sum();
}

int main() {
    return gcd(48, 18);
}
