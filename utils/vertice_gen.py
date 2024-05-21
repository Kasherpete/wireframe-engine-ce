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
                return list(filter(None, line.split(' ')))[2]

        raise Exception(f'No constant {target_string} found')
    
def find_powAZx50_list():

    with open(file_path, 'r') as file:
        for line in file:
            if f'const static uint8_t powAZx50list[] =' in line:
                value = line.split('{')[1].split('}')[0].split(',')
                for i in range(len(value)):
                    value[i] = int(value[i])
                return value
            elif f'const static uint24_t powAZx50list[] =' in line:
                value = line.split('{')[1].split('}')[0].split(',')
                for i in range(len(value)):
                    value[i] = int(value[i])
                return value

        raise Exception(f'powAZx50 list not found')



front = int(find_constant('RENDER_DIST_Z_FRONT'))
back = int(find_constant('RENDER_DIST_Z_BACK'))
fov = float(find_constant('FOV'))
max_x = int(find_constant('RENDER_DIST_X'))
gfx_width_half = int(find_constant('GFX_WIDTH_HALF'))

print(f'RENDER_DIST_Z_FRONT: {front}')
print(f'RENDER_DIST_Z_BACK: {back}')
print(f'FOV: {fov}\n')
print(f'RENDER_DIST_X: {max_x}\n')
print(f'GFX_WIDTH_HALF: {gfx_width_half}')


list1 = []

array_type = int(input("Would you like to use type uint8_t or uint24_t? Recommended is 24. (8/24): "))

if array_type == 24:
    maximum = 16777216
elif array_type == 8:
    maximum = 255
else:
    raise Exception('Incorrect value!')

# LOGIC HERE

powAZx50 = find_powAZx50_list()

print(powAZx50)

value = []

for z in range(len(powAZx50)):
    z -= back
    value.append([])
    for i in range(-max_x, max_x):
        if array_type == 8:
            value[z+back].append(max(0,min(maximum, (i*powAZx50[z+back]) + gfx_width_half)))
        else:
            value[z+back].append(min(maximum, (i*powAZx50[z+back]) + gfx_width_half))


raw_value = str(value).replace('[', '{').replace(']', '}\n')[1:-1]
print(raw_value)
print(f'size of array in bytes: {(len(value[0])*len(value))*3}')
print(f'array is {len(value[0])}x{len(value)}.')

with open(f"{pathlib.Path(__file__).parent.resolve()}/vertexCacheList.inc", 'w') as file:
    file.write(raw_value)
    file.flush()
    

print('Finished configuring engine pre-calculations!')
print('IMPORTANT: Remember to update variable types accordingly!!')
