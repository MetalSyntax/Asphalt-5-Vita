import re

with open("source/generated_jni_table.h", "r") as f:
    content = f.read()

methods = []
for line in content.split("\n"):
    match = re.search(r'extern (.*?)\s+stub_(.*?)_(\d+)\(jmethodID id', line)
    if match:
        ret_type = match.group(1).strip()
        name = match.group(2)
        idx = match.group(3)
        # Split out class and method
        parts = name.split('_')
        if len(parts) >= 2:
            cls = parts[0]
            meth = parts[1]
            if ret_type == 'jboolean': t = 'BOOLEAN'
            elif ret_type == 'jbyte': t = 'BYTE'
            elif ret_type == 'jchar': t = 'CHAR'
            elif ret_type == 'jdouble': t = 'DOUBLE'
            elif ret_type == 'jfloat': t = 'FLOAT'
            elif ret_type == 'jint': t = 'INT'
            elif ret_type == 'jlong': t = 'LONG'
            elif ret_type == 'jshort': t = 'SHORT'
            elif ret_type == 'void': t = 'VOID'
            else: t = 'OBJECT'
            
            methods.append({
                'id': idx,
                'name': meth,
                'type': t,
                'stub': f"stub_{name}_{idx}"
            })

with open("source/java.c", "w") as f:
    f.write('#include <falso_jni/FalsoJNI_Impl.h>\n')
    f.write('#include "generated_jni_table.h"\n\n')
    
    f.write('NameToMethodID nameToMethodId[] = {\n')
    for m in methods:
        f.write(f'\t{{ {m["id"]}, "{m["name"]}", METHOD_TYPE_{m["type"]} }},\n')
    f.write('};\n\n')
    
    types = ['Boolean', 'Byte', 'Char', 'Double', 'Float', 'Int', 'Long', 'Object', 'Short', 'Void']
    for type_name in types:
        f.write(f'Methods{type_name} methods{type_name}[] = {{\n')
        for m in methods:
            if m['type'] == type_name.upper():
                f.write(f'\t{{ {m["id"]}, {m["stub"]} }},\n')
        f.write('};\n\n')
        
    f.write('''
// System-wide constant that applications sometimes request
char WINDOW_SERVICE[] = "window";
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
\t{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT }, 
\t{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
\t{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
\t{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
''')

