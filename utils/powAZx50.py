import math
import pathlib


file_path = f"{pathlib.Path(__file__).parent.resolve()}/../src/main.c"

try:
    open(file_path)
except FileNotFoundError:
    file_path = '../src/main.c'


def find_constant(target_string):
  
    with open(file_path, 'r') as file:
        for line in file:
            if f'#define {target_string}' in line:
                return line.split(' ')[2]

        raise Exception(f'No constant {target_string} found')


def replace_string_in_file(target_string, replacement_string):
    updated_lines = []
    value = False

    with open(file_path, 'r') as file:
        for line in file:
            if target_string in line:
                line = replacement_string
                value = True
            updated_lines.append(line)

    with open(file_path, 'w') as file:
        file.writelines(updated_lines)

    return value



front = int(find_constant('RENDER_DIST_Z_FRONT'))
back = int(find_constant('RENDER_DIST_Z_BACK'))
fov = float(find_constant('FOV'))

print(f'RENDER_DIST_Z_FRONT: {front}')
print(f'RENDER_DIST_Z_BACK: {back}')
print(f'FOV: {fov}\n')


list1 = []

array_type = int(input("Would you like to use type uint8_t or uint24_t? (8/24): "))

if array_type == 24:
    maximum = 16777216
elif array_type == 8:
    maximum = 255
else:
    raise Exception('Incorrect value!')

for i in range(-back, front):
    list1.append(min(round(pow(fov, i)*50), maximum))

value = f"{{{str(list1)[1:-1]}}}"

print(f'\n{value}\n')

if not replace_string_in_file(f'uint8_t powAZx50list[]', f'uint{array_type}_t powAZx50list[] = {value};\n'):
    if not replace_string_in_file(f'uint24_t powAZx50list[]', f'uint{array_type}_t powAZx50list[] = {value};\n'):
        raise Exception('Error while inserting powAZx50 array into the file!')

print('Finished configuring engine pre-calculations!')
print('IMPORTANT: Remember to update variable types accordingly!!')
