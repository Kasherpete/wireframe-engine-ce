import math

a = int(input("Enter the value of RENDER_DISTANCE_Z_BACK: "))
b = int(input("Enter the value of RENDER_DISTANCE_Z_FRONT: "))

list1 = []

for i in range(a, b):
    list1.append(round(pow(.8, i)*50))
    
print(f"{{{str(list1)[1:-1]}}}")
