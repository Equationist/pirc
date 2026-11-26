-- Negative test: P3-7.11 body for derived task type
procedure Neg_Body_Der_Task is
   task type T;
   task body T is begin null; end T;
   type DT is new T;
   task body DT is begin null; end DT;
begin
   null;
end Neg_Body_Der_Task;
