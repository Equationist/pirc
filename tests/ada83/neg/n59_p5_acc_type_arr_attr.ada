procedure N59_Acc_Type_Arr_Attr is
   type A is array (1 .. 5) of Integer;
   type Acc is access A;
   X : Integer := Acc'First;
begin
   null;
end N59_Acc_Type_Arr_Attr;
