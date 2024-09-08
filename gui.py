import tkinter as tk
from tkinter import messagebox

def calculate():
    try:
        user_input = float(entry.get())
        result = user_input * 2  # Example: multiplying the input by 2
        result_label.config(text=f"Result: {result}")
    except ValueError:
        messagebox.showerror("Invalid Input", "Please enter a valid number")

# Create the main window
root = tk.Tk()
root.title("Simple Input-Output GUI")

# Create an input field
tk.Label(root, text="Enter a number:").pack(pady=10)
entry = tk.Entry(root)
entry.pack(pady=10)

# Create a button to process the input
tk.Button(root, text="Calculate", command=calculate).pack(pady=10)

# Create a label to show the result
result_label = tk.Label(root, text="Result:")
result_label.pack(pady=10)

# Start the GUI event loop
root.mainloop()
