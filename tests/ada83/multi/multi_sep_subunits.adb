separate (Multi_Sep_Pkg)
function Sub_Add (A, B : Integer) return Integer is
begin
   return A + B + Base_Var;
end Sub_Add;

separate (Multi_Sep_Pkg)
procedure Raise_From_Sub is
begin
   raise My_Cross_Exc;
end Raise_From_Sub;

separate (Multi_Sep_Pkg)
task body Sub_Task is
begin
   accept Ping (X : Integer; Y : out Integer) do
      Y := X + Read_Const;
   end Ping;
end Sub_Task;
