set terminal png size 2000,2000
set output 'append_only.png'


set style data points
set style line 1 lc rgb '#FF0000' pt 7 ps 0.5  # Red points for meque
set style line 2 lc rgb '#00FF00' pt 7 ps 0.5  # Green points for deque
set style line 3 lc rgb '#0000FF' pt 7 ps 0.5  # Blue points for list

# Set up the fit functions
meque_f(x) = meque_a*x + meque_b  # Linear fit for meque
deque_f(x) = deque_a*x + deque_b  # Linear fit for deque
list_f(x) = list_a*x + list_b  # Linear fit for list

# Fit the data
fit meque_f(x) 'append_only.meque.data' using 2:3 via meque_a,meque_b
fit deque_f(x) 'append_only.deque.data' using 2:3 via deque_a,deque_b
fit list_f(x) 'append_only.list.data' using 2:3 via list_a,list_b


# Add fit parameters to plot
set label 1 sprintf("meque: y = %.2e x + %.2e", meque_a, meque_b) at graph 0.05,0.95
set label 2 sprintf("deque: y = %.2e x + %.2e", deque_a, deque_b) at graph 0.05,0.90
set label 3 sprintf("list: y = %.2e x + %.2e", list_a, list_b) at graph 0.05,0.85


# Plot the data and fits
plot meque_f(x) title 'meque fit' with lines ls 1 lw 3 dt 1, \
     'append_only.meque.data' using 2:3 title 'meque' with points ls 1 pt 7 ps 0.1, \
     deque_f(x) title 'deque fit' with lines ls 2 lw 3 dt 2, \
     'append_only.deque.data' using 2:3 title 'deque' with points ls 2 pt 7 ps 0.1, \
     list_f(x) title 'list fit' with lines ls 3 lw 3 dt 3, \
     'append_only.list.data' using 2:3 title 'list' with points ls 3 pt 7 ps 0.1

# # Plot the fits
# plot meque_f(x) title 'meque fit' with lines ls 1, \
#      deque_f(x) title 'deque fit' with lines ls 2, \
#      list_f(x) title 'list fit' with lines ls 3

# Set labels and title
set xlabel 'Number of elements'
set ylabel 'Time (seconds)'
set title 'Performance Comparison of meque, deque, and list'

# Print fit parameters to terminal
print "meque: a = ", meque_a, ", b = ", meque_b
print "deque: a = ", deque_a, ", b = ", deque_b
print "list: a = ", list_a, ", b = ", list_b
